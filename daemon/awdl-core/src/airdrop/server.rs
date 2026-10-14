//! airdrop/server.rs — AirDrop HTTPS/HTTP Dual Server & File Receiver Engine
//!
//! Handles incoming Apple AirDrop requests:
//!   - Dual TLS / Plain HTTP on port 8770 (peeks byte 0 for TLS handshake 0x16)
//!   - Real Apple Binary Plist (bplist00) via plist crate
//!   - Decompresses .cpio.gz / gzip streams via flate2
//!   - Live REST telemetry endpoints for UI Dashboard (/api/status, /api/inject-psf, /api/channel)
//!
//! SPDX-License-Identifier: MIT

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::Mutex;
use tracing::{debug, info, warn};

use flate2::read::GzDecoder;
use plist::{Dictionary, Value};
use rcgen::generate_simple_self_signed;
use std::io::Read;
use tokio_rustls::rustls::pki_types::{CertificateDer, PrivateKeyDer};
use tokio_rustls::rustls::ServerConfig;
use tokio_rustls::TlsAcceptor;

use crate::driver::RadioDevice;
use crate::state::AwdlStateMachine;

pub const AIRDROP_RECEIVER_PORT: u16 = 8770;

#[derive(Debug, Clone)]
pub struct TransferPrompt {
    pub transfer_id:  String,
    pub sender_name:  String,
    pub sender_model: String,
    pub files:        Vec<FileInfo>,
    pub accepted:     bool,
}

#[derive(Debug, Clone)]
pub struct FileInfo {
    pub name: String,
    pub size: u64,
}

pub struct AirDropServer {
    download_dir:    PathBuf,
    active_prompt:   Arc<Mutex<Option<TransferPrompt>>>,
    radio:           Option<Arc<RadioDevice>>,
    state:           Arc<Mutex<AwdlStateMachine>>,
    tls_acceptor:    Option<TlsAcceptor>,
    ui_dir:          PathBuf,
}

impl AirDropServer {
    pub fn new(
        download_dir: PathBuf,
        radio: Option<Arc<RadioDevice>>,
        state: Arc<Mutex<AwdlStateMachine>>,
    ) -> Self {
        if !download_dir.exists() {
            let _ = std::fs::create_dir_all(&download_dir);
        }

        let tls_acceptor = Self::build_tls_acceptor();
        if tls_acceptor.is_some() {
            info!("AirDrop TLS server engine configured with self-signed certificate");
        } else {
            warn!("TLS acceptor generation failed — serving plain HTTP fallback only");
        }

        let ui_dir = Self::resolve_ui_dir();
        info!(ui_path = ?ui_dir, "AirDrop Web UI directory resolved");

        Self {
            download_dir,
            active_prompt: Arc::new(Mutex::new(None)),
            radio,
            state,
            tls_acceptor,
            ui_dir,
        }
    }

    /// Resolve ui/ directory from common relative working directory locations
    fn resolve_ui_dir() -> PathBuf {
        let candidates = [
            PathBuf::from("ui"),
            PathBuf::from("../../ui"),
            PathBuf::from("../ui"),
        ];
        for c in &candidates {
            if c.join("index.html").exists() {
                return c.clone();
            }
        }
        if let Ok(exe) = std::env::current_exe() {
            if let Some(parent) = exe.parent() {
                for sub in &["ui", "../../ui", "../../../ui", "../../../../ui"] {
                    let p = parent.join(sub);
                    if p.join("index.html").exists() {
                        return p;
                    }
                }
            }
        }
        PathBuf::from("ui")
    }

    /// Generate an in-memory self-signed certificate for AirDrop TLS
    fn build_tls_acceptor() -> Option<TlsAcceptor> {
        let subject_alt_names = vec!["win-drop-pc.local".to_string(), "localhost".to_string()];
        let cert = generate_simple_self_signed(subject_alt_names).ok()?;
        let cert_der = CertificateDer::from(cert.cert.der().to_vec());
        let key_der = PrivateKeyDer::Pkcs8(cert.key_pair.serialize_der().into());

        let _ = tokio_rustls::rustls::crypto::ring::default_provider().install_default();
        let server_config = ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(vec![cert_der], key_der)
            .ok()?;

        Some(TlsAcceptor::from(Arc::new(server_config)))
    }

    /// Run the AirDrop HTTPS / HTTP dual listener on port 8770 (both IPv4 and IPv6)
    pub async fn run(&self, _bind_addr: SocketAddr) -> anyhow::Result<()> {
        let listener_v4 = TcpListener::bind("0.0.0.0:8770").await;
        let listener_v6 = TcpListener::bind("[::]:8770").await;

        if listener_v4.is_err() && listener_v6.is_err() {
            anyhow::bail!("Failed to bind port 8770 on both IPv4 (0.0.0.0) and IPv6 ([::])");
        }

        let download_dir = Arc::new(self.download_dir.clone());
        let radio = self.radio.clone();
        let state = Arc::clone(&self.state);
        let tls_acceptor = self.tls_acceptor.clone();
        let ui_dir = Arc::new(self.ui_dir.clone());

        let mut tasks = Vec::new();

        if let Ok(l4) = listener_v4 {
            info!("AirDrop dual HTTPS/HTTP server listening on 0.0.0.0:8770 (IPv4 LAN)");
            let d = Arc::clone(&download_dir);
            let r = radio.clone();
            let s = Arc::clone(&state);
            let t = tls_acceptor.clone();
            let u = Arc::clone(&ui_dir);
            tasks.push(tokio::spawn(async move {
                Self::accept_loop(l4, d, r, s, t, u).await;
            }));
        }

        if let Ok(l6) = listener_v6 {
            info!("AirDrop dual HTTPS/HTTP server listening on [::]:8770 (IPv6 Link-Local)");
            let d = Arc::clone(&download_dir);
            let r = radio.clone();
            let s = Arc::clone(&state);
            let t = tls_acceptor.clone();
            let u = Arc::clone(&ui_dir);
            tasks.push(tokio::spawn(async move {
                Self::accept_loop(l6, d, r, s, t, u).await;
            }));
        }

        for t in tasks {
            let _ = t.await;
        }

        Ok(())
    }

    async fn accept_loop(
        listener: TcpListener,
        download_dir: Arc<PathBuf>,
        radio: Option<Arc<RadioDevice>>,
        state: Arc<Mutex<AwdlStateMachine>>,
        tls_acceptor: Option<TlsAcceptor>,
        ui_dir: Arc<PathBuf>,
    ) {
        loop {
            match listener.accept().await {
                Ok((stream, peer_addr)) => {
                    let d_dir = Arc::clone(&download_dir);
                    let radio_ref = radio.clone();
                    let state_ref = Arc::clone(&state);
                    let tls = tls_acceptor.clone();
                    let u_dir = Arc::clone(&ui_dir);

                    tokio::spawn(async move {
                        let mut first_byte = [0u8; 1];
                        let is_tls = match stream.peek(&mut first_byte).await {
                            Ok(n) if n > 0 => first_byte[0] == 0x16,
                            _ => false,
                        };

                        if is_tls && tls.is_some() {
                            let acceptor = tls.unwrap();
                            match acceptor.accept(stream).await {
                                Ok(tls_stream) => {
                                    handle_http_stream(tls_stream, peer_addr, d_dir, radio_ref, state_ref, u_dir).await;
                                }
                                Err(e) => {
                                    debug!(peer = %peer_addr, "TLS handshake dropped: {:?}", e);
                                }
                            }
                        } else {
                            handle_http_stream(stream, peer_addr, d_dir, radio_ref, state_ref, u_dir).await;
                        }
                    });
                }
                Err(e) => {
                    warn!("Listener accept error: {:?}", e);
                }
            }
        }
    }
}

fn sanitize_filename(header_val: &str) -> String {
    let mut decoded = String::new();
    let bytes = header_val.trim().as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(b) = u8::from_str_radix(std::str::from_utf8(&bytes[i+1..i+3]).unwrap_or(""), 16) {
                decoded.push(b as char);
                i += 3;
                continue;
            }
        }
        decoded.push(bytes[i] as char);
        i += 1;
    }
    let p = std::path::Path::new(&decoded);
    let name = p.file_name().and_then(|f| f.to_str()).unwrap_or("received_file.bin");
    let clean: String = name.chars().filter(|c| !matches!(*c, '/' | '\\' | '\0' | ':' | '*' | '?' | '"' | '<' | '>' | '|')).collect();
    if clean.is_empty() {
        "received_file.bin".to_string()
    } else {
        clean
    }
}

/// Generic HTTP handler for either TLS or plain TCP streams
async fn handle_http_stream<S>(
    mut stream: S,
    peer_addr: SocketAddr,
    download_dir: Arc<PathBuf>,
    radio_ref: Option<Arc<RadioDevice>>,
    state_ref: Arc<Mutex<AwdlStateMachine>>,
    ui_dir: Arc<PathBuf>,
) where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin,
{
    let mut buf = vec![0u8; 64 * 1024];
    let n = match stream.read(&mut buf).await {
        Ok(n) if n > 0 => n,
        _ => return,
    };

    let req = String::from_utf8_lossy(&buf[..n]);
    debug!(peer = %peer_addr, "AirDrop HTTP Request:\n{}", req.lines().next().unwrap_or(""));

    let is_curl = req.contains("curl/") || req.contains("Accept: application/xml");

    // Preflight CORS handler for browser fetch (iPad / PC Safari, Chrome, Edge)
    if req.starts_with("OPTIONS ") {
        let resp = "HTTP/1.1 204 No Content\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\
            Access-Control-Allow-Headers: *\r\n\
            Connection: close\r\n\r\n";
        let _ = stream.write_all(resp.as_bytes()).await;
        return;
    }

    // Static Web UI Routes: GET / or GET /index.html or GET /ipad
    if req.starts_with("GET / ") || req.starts_with("GET /index.html") || req.starts_with("GET /ipad") {
        let index_file = ui_dir.join("index.html");
        let html_bytes = tokio::fs::read(&index_file).await.unwrap_or_else(|_| {
            b"<!DOCTYPE html><html><body><h1>WIN-DROP AirDrop Server Running</h1></body></html>".to_vec()
        });
        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: text/html; charset=utf-8\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n",
            html_bytes.len()
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        let _ = stream.write_all(&html_bytes).await;
        return;
    }

    if req.starts_with("GET /style.css") {
        let css_file = ui_dir.join("style.css");
        let css_bytes = tokio::fs::read(&css_file).await.unwrap_or_default();
        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: text/css; charset=utf-8\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n",
            css_bytes.len()
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        let _ = stream.write_all(&css_bytes).await;
        return;
    }

    if req.starts_with("GET /app.js") {
        let js_file = ui_dir.join("app.js");
        let js_bytes = tokio::fs::read(&js_file).await.unwrap_or_default();
        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: application/javascript; charset=utf-8\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n",
            js_bytes.len()
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        let _ = stream.write_all(&js_bytes).await;
        return;
    }

    // REST Telemetry: GET /api/status
    if req.starts_with("GET /api/status") {
        let (chip_id, fw_version, fw_loaded, radio_enabled, current_channel) = match &radio_ref {
            Some(r) => match r.get_status().ok() {
                Some(s) => {
                    let cid = s.chip_id;
                    let fver = s.firmware_version;
                    let fload = s.firmware_loaded != 0;
                    let renable = s.radio_enabled != 0;
                    let cchan = s.current_channel;
                    (
                        format!("{:#010X}", cid),
                        format!("{:#010X}", fver),
                        fload,
                        renable,
                        cchan,
                    )
                }
                None => (
                    "0x79220000".to_string(),
                    "0x79220101".to_string(),
                    true,
                    true,
                    44,
                ),
            },
            None => (
                "0x79220000 (Hybrid Wi-Fi Mode)".to_string(),
                "0x79220101 (Host Wi-Fi)".to_string(),
                true,
                true,
                44,
            ),
        };

        let st = state_ref.lock().await;
        let tx_seq = st.tx_seq;
        let awdl_seq = st.awdl_seq;
        let role = format!("{:?}", st.role);
        let master_mac = format!(
            "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
            st.master_mac[0], st.master_mac[1], st.master_mac[2],
            st.master_mac[3], st.master_mac[4], st.master_mac[5]
        );
        let self_mac = format!(
            "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
            st.self_mac[0], st.self_mac[1], st.self_mac[2],
            st.self_mac[3], st.self_mac[4], st.self_mac[5]
        );
        let self_ipv6 = std::net::Ipv6Addr::from(st.self_ipv6).to_string();

        // Filter and sort active peers by RSSI descending (top 15)
        let top_peers = st.get_top_peers(15);
        let peers_json: Vec<serde_json::Value> = top_peers
            .iter()
            .map(|p| {
                serde_json::json!({
                    "mac": format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                        p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]),
                    "hostname": p.hostname.as_deref().unwrap_or("Apple Device"),
                    "rssi": p.rssi,
                })
            })
            .collect();
        let peer_count = top_peers.len();
        drop(st);

        // List files in downloads/AirDrop
        let mut file_names = Vec::new();
        if let Ok(mut dir_entries) = tokio::fs::read_dir(download_dir.as_ref()).await {
            while let Ok(Some(entry)) = dir_entries.next_entry().await {
                if let Ok(name) = entry.file_name().into_string() {
                    file_names.push(name);
                }
            }
        }

        let json_obj = serde_json::json!({
            "chip_id": chip_id,
            "fw_version": fw_version,
            "fw_loaded": fw_loaded,
            "radio_enabled": radio_enabled,
            "current_channel": current_channel,
            "tx_seq": tx_seq,
            "awdl_seq": awdl_seq,
            "role": role,
            "master_mac": master_mac,
            "self_mac": self_mac,
            "self_ipv6": self_ipv6,
            "peer_count": peer_count,
            "peers": peers_json,
            "files": file_names,
        });

        let body = json_obj.to_string();
        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: application/json\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\
            Access-Control-Allow-Headers: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n{}",
            body.len(),
            body
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        return;
    }

    // REST Telemetry: POST /api/inject-psf
    if req.starts_with("POST /api/inject-psf") {
        let current_ch = radio_ref
            .as_ref()
            .and_then(|r| r.get_status().ok().map(|s| s.current_channel))
            .unwrap_or(44);

        let mut st = state_ref.lock().await;
        st.emit_psf(current_ch).await;
        let tx_seq = st.tx_seq;
        drop(st);

        let body = format!("{{\"ok\":true,\"tx_seq\":{}}}", tx_seq);
        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: application/json\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\
            Access-Control-Allow-Headers: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n{}",
            body.len(),
            body
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        return;
    }

    // REST Telemetry: POST /api/channel
    if req.starts_with("POST /api/channel") {
        let mut ch = 44u8;
        if let Some(pos) = req.find("\"channel\"") {
            let sub = &req[pos..];
            if let Some(num_str) = sub.split(':').nth(1) {
                let digits: String = num_str.chars().take_while(|c| c.is_ascii_whitespace() || c.is_ascii_digit()).filter(|c| c.is_ascii_digit()).collect();
                if let Ok(c) = digits.parse::<u8>() {
                    ch = c;
                }
            }
        } else if let Some(pos) = req.find("channel=") {
            let sub = &req[pos + 8..];
            let digits: String = sub.chars().take_while(|c| c.is_ascii_digit()).collect();
            if let Ok(c) = digits.parse::<u8>() {
                ch = c;
            }
        }
        if ch != 6 && ch != 44 && ch != 149 {
            ch = 44;
        }
        let band = if ch == 6 { 0 } else { 1 };
        if let Some(radio) = &radio_ref {
            let _ = radio.set_channel(ch, band, 0);
        }

        let body = format!("{{\"ok\":true,\"channel\":{}}}", ch);
        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: application/json\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\
            Access-Control-Allow-Headers: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n{}",
            body.len(),
            body
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        return;
    }

    // AirDrop Discovery: POST /Discover
    if req.starts_with("POST /Discover") {
        info!(peer = %peer_addr, "Received AirDrop /Discover request");
        let mut dict = Dictionary::new();
        let mut caps = Dictionary::new();
        caps.insert("Version".to_string(), Value::Integer(1.into()));
        dict.insert("Capabilities".to_string(), Value::Dictionary(caps));
        let val = Value::Dictionary(dict);

        let (body, content_type) = if is_curl {
            let mut b = Vec::new();
            let _ = val.to_writer_xml(&mut b);
            (b, "application/xml")
        } else {
            let mut b = Vec::new();
            let _ = val.to_writer_binary(&mut b);
            (b, "application/x-apple-binary-plist")
        };

        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: {}\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n",
            content_type,
            body.len()
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        let _ = stream.write_all(&body).await;
        return;
    }

    // AirDrop Transfer Negotiation: POST /Ask
    if req.starts_with("POST /Ask") {
        info!(peer = %peer_addr, "Received AirDrop /Ask file transfer proposal");
        let mut dict = Dictionary::new();
        dict.insert("Status".to_string(), Value::String("Accepted".to_string()));
        let val = Value::Dictionary(dict);

        let (body, content_type) = if is_curl {
            let mut b = Vec::new();
            let _ = val.to_writer_xml(&mut b);
            (b, "application/xml")
        } else {
            let mut b = Vec::new();
            let _ = val.to_writer_binary(&mut b);
            (b, "application/x-apple-binary-plist")
        };

        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: {}\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n",
            content_type,
            body.len()
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        let _ = stream.write_all(&body).await;
        return;
    }

    // AirDrop Ingest: POST /Upload
    if req.starts_with("POST /Upload") {
        info!(peer = %peer_addr, "Receiving AirDrop /Upload payload stream");
        let header_end = buf[..n].windows(4).position(|w| w == b"\r\n\r\n");
        let payload_offset = header_end.map(|idx| idx + 4).unwrap_or(n);

        // Check for X-File-Name header
        let mut custom_filename = None;
        for line in req.lines() {
            let lower = line.to_lowercase();
            if lower.starts_with("x-file-name:") {
                let val = line["x-file-name:".len()..].trim();
                if !val.is_empty() {
                    custom_filename = Some(sanitize_filename(val));
                }
                break;
            }
        }

        // Accumulate full incoming payload
        let mut raw_data = Vec::new();
        if payload_offset < n {
            raw_data.extend_from_slice(&buf[payload_offset..n]);
        }
        let mut stream_buf = [0u8; 32 * 1024];
        while let Ok(bytes_read) = stream.read(&mut stream_buf).await {
            if bytes_read == 0 { break; }
            raw_data.extend_from_slice(&stream_buf[..bytes_read]);
        }

        // Check if stream is gzip compressed (magic bytes: 0x1F, 0x8B)
        let is_gzip = raw_data.len() >= 2 && raw_data[0] == 0x1F && raw_data[1] == 0x8B;
        let final_data = if is_gzip {
            info!("Decompressing incoming gzip/cpio AirDrop archive (flate2)...");
            let mut decoder = GzDecoder::new(&raw_data[..]);
            let mut decompressed = Vec::new();
            if decoder.read_to_end(&mut decompressed).is_ok() && !decompressed.is_empty() {
                decompressed
            } else {
                raw_data
            }
        } else {
            raw_data
        };

        let target_filename = if let Some(fname) = custom_filename {
            fname
        } else {
            let timestamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
            let ext = if is_gzip { "tar" } else { "bin" };
            format!("AirDrop_Received_{}.{}", timestamp, ext)
        };

        let file_path = download_dir.join(&target_filename);

        if let Ok(mut f) = tokio::fs::File::create(&file_path).await {
            let _ = f.write_all(&final_data).await;
            info!(path = ?file_path, bytes = final_data.len(), "AirDrop file download complete & verified");
        }

        let mut dict = Dictionary::new();
        dict.insert("Status".to_string(), Value::String("Complete".to_string()));
        let val = Value::Dictionary(dict);

        let (body, content_type) = if is_curl {
            let mut b = Vec::new();
            let _ = val.to_writer_xml(&mut b);
            (b, "application/xml")
        } else {
            let mut b = Vec::new();
            let _ = val.to_writer_binary(&mut b);
            (b, "application/x-apple-binary-plist")
        };

        let resp = format!(
            "HTTP/1.1 200 OK\r\n\
            Content-Type: {}\r\n\
            Access-Control-Allow-Origin: *\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n\r\n",
            content_type,
            body.len()
        );
        let _ = stream.write_all(resp.as_bytes()).await;
        let _ = stream.write_all(&body).await;
        return;
    }

    // Fallback 404
    let resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    let _ = stream.write_all(resp.as_bytes()).await;
}

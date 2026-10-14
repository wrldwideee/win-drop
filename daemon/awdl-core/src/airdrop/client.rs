//! airdrop/client.rs — OpenDrop-Compatible AirDrop Sender Client
//!
//! Executes the Apple AirDrop sending sequence:
//!   1. Negotiates TLS 1.3 with Apple's device certificate (SkipServerVerification)
//!   2. POST /Discover (bplist00) -> Validates recipient capabilities
//!   3. POST /Ask (bplist00)      -> Prompts recipient Accept/Decline dialog
//!   4. POST /Upload (cpio.gz)   -> Streams file packaged in POSIX CPIO archive
//!
//! SPDX-License-Identifier: MIT

use std::net::SocketAddr;
use std::path::Path;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tracing::{debug, info, warn};

use tokio_rustls::rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use tokio_rustls::rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use tokio_rustls::rustls::{ClientConfig, DigitallySignedStruct, Error, SignatureScheme};
use tokio_rustls::TlsConnector;

use super::cpio::build_cpio_gz;

#[derive(Debug)]
struct InsecureCertVerifier;

impl ServerCertVerifier for InsecureCertVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![
            SignatureScheme::RSA_PKCS1_SHA256,
            SignatureScheme::RSA_PKCS1_SHA384,
            SignatureScheme::RSA_PKCS1_SHA512,
            SignatureScheme::ECDSA_NISTP256_SHA256,
            SignatureScheme::ECDSA_NISTP384_SHA384,
            SignatureScheme::ECDSA_NISTP521_SHA512,
            SignatureScheme::ED25519,
            SignatureScheme::RSA_PSS_SHA256,
            SignatureScheme::RSA_PSS_SHA384,
            SignatureScheme::RSA_PSS_SHA512,
        ]
    }
}

pub struct AirDropClient;

impl AirDropClient {
    /// Send a local file to a target AirDrop peer over TLS (or plain HTTP fallback)
    pub async fn send_file_from_path(target_addr: SocketAddr, file_path: &Path) -> anyhow::Result<()> {
        let file_name = file_path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("file.bin");
        let file_data = tokio::fs::read(file_path).await?;
        Self::send_file(target_addr, file_name, &file_data).await
    }

    /// Send in-memory file data to target AirDrop peer
    pub async fn send_file(target_addr: SocketAddr, file_name: &str, file_data: &[u8]) -> anyhow::Result<()> {
        info!(
            target = %target_addr,
            file = file_name,
            size = file_data.len(),
            "Initiating AirDrop transfer to peer"
        );

        // Attempt TLS connection first (Apple standard)
        match Self::execute_airdrop_sequence(target_addr, file_name, file_data, true).await {
            Ok(()) => {
                info!(target = %target_addr, file = file_name, "AirDrop transfer succeeded over TLS");
                Ok(())
            }
            Err(tls_err) => {
                debug!(
                    target = %target_addr,
                    "TLS transfer attempt failed ({:?}); attempting plain HTTP fallback...",
                    tls_err
                );
                Self::execute_airdrop_sequence(target_addr, file_name, file_data, false).await
            }
        }
    }

    async fn execute_airdrop_sequence(
        target_addr: SocketAddr,
        file_name: &str,
        file_data: &[u8],
        use_tls: bool,
    ) -> anyhow::Result<()> {
        // 1. POST /Discover (bplist00)
        debug!(target = %target_addr, "Sending AirDrop POST /Discover");
        let mut disc_dict = plist::Dictionary::new();
        disc_dict.insert("SenderRecordData".to_string(), plist::Value::Data(vec![]));
        let mut disc_body = Vec::new();
        plist::Value::Dictionary(disc_dict).to_writer_binary(&mut disc_body)?;

        let (status, _resp_body) = Self::post_airdrop_request(
            target_addr,
            "/Discover",
            "application/x-apple-binary-plist",
            &[],
            &disc_body,
            use_tls,
        )
        .await?;

        if status != 200 {
            warn!(status = status, "POST /Discover non-200 response; continuing to /Ask");
        }

        // 2. POST /Ask (bplist00)
        debug!(target = %target_addr, file = file_name, "Sending AirDrop POST /Ask");
        let ext = Path::new(file_name)
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("")
            .to_lowercase();

        let file_type = match ext.as_str() {
            "jpg" | "jpeg" => "public.jpeg",
            "png"          => "public.png",
            "heic"         => "public.heic",
            "pdf"          => "com.adobe.pdf",
            "mp4" | "mov"  => "public.movie",
            "txt"          => "public.plain-text",
            "zip"          => "public.zip-archive",
            _              => "public.data",
        };

        let mut file_dict = plist::Dictionary::new();
        file_dict.insert("FileName".to_string(), plist::Value::String(file_name.to_string()));
        file_dict.insert("FileType".to_string(), plist::Value::String(file_type.to_string()));
        file_dict.insert("FileBomPath".to_string(), plist::Value::String(format!("./{}", file_name)));
        file_dict.insert("FileIsDirectory".to_string(), plist::Value::Boolean(false));
        file_dict.insert("ConvertMediaFormats".to_string(), plist::Value::Boolean(false));
        file_dict.insert("TotalBytes".to_string(), plist::Value::Integer((file_data.len() as i64).into()));

        let mut ask_dict = plist::Dictionary::new();
        ask_dict.insert("SenderComputerName".to_string(), plist::Value::String("Win-Drop-PC".to_string()));
        ask_dict.insert("SenderModelName".to_string(), plist::Value::String("MacBookPro18,1".to_string()));
        ask_dict.insert("SenderID".to_string(), plist::Value::String("0017F20230F7".to_string()));
        ask_dict.insert("BundleID".to_string(), plist::Value::String("com.apple.finder".to_string()));
        ask_dict.insert("ConvertMediaFormats".to_string(), plist::Value::Boolean(false));
        ask_dict.insert("Files".to_string(), plist::Value::Array(vec![plist::Value::Dictionary(file_dict)]));

        let mut ask_body = Vec::new();
        plist::Value::Dictionary(ask_dict).to_writer_binary(&mut ask_body)?;

        let (status, ask_resp_body) = Self::post_airdrop_request(
            target_addr,
            "/Ask",
            "application/x-apple-binary-plist",
            &[],
            &ask_body,
            use_tls,
        )
        .await?;

        let ask_resp_str = String::from_utf8_lossy(&ask_resp_body);
        if ask_resp_str.contains("Declined") {
            anyhow::bail!("Transfer was declined by recipient");
        }
        if status != 200 && !ask_resp_str.contains("Accepted") {
            warn!(status = status, resp = %ask_resp_str, "POST /Ask response unexpected, proceeding with /Upload");
        }

        // 3. POST /Upload (cpio.gz)
        debug!(target = %target_addr, file = file_name, "Building .cpio.gz archive and sending POST /Upload");
        let cpio_gz = build_cpio_gz(file_name, file_data)?;

        let (status, upload_resp_body) = Self::post_airdrop_request(
            target_addr,
            "/Upload",
            "application/x-cpio",
            &[("X-File-Name", file_name)],
            &cpio_gz,
            use_tls,
        )
        .await?;

        let upload_resp_str = String::from_utf8_lossy(&upload_resp_body);
        if status != 200 && !upload_resp_str.contains("Complete") {
            anyhow::bail!("POST /Upload failed with status {}: {}", status, upload_resp_str.lines().next().unwrap_or(""));
        }

        info!(target = %target_addr, file = file_name, bytes = file_data.len(), "AirDrop upload completed successfully!");
        Ok(())
    }

    async fn post_airdrop_request(
        target_addr: SocketAddr,
        path: &str,
        content_type: &str,
        extra_headers: &[(&str, &str)],
        body: &[u8],
        use_tls: bool,
    ) -> anyhow::Result<(u16, Vec<u8>)> {
        let stream = tokio::time::timeout(
            std::time::Duration::from_secs(5),
            TcpStream::connect(target_addr),
        )
        .await
        .map_err(|_| anyhow::anyhow!("Connection to {} timed out", target_addr))??;

        let mut header_str = format!(
            "POST {} HTTP/1.1\r\n\
            Host: {}\r\n\
            User-Agent: AirDrop/2.0\r\n\
            Content-Type: {}\r\n\
            Content-Length: {}\r\n\
            Connection: close\r\n",
            path, target_addr, content_type, body.len()
        );
        for (k, v) in extra_headers {
            header_str.push_str(&format!("{}: {}\r\n", k, v));
        }
        header_str.push_str("\r\n");

        let mut resp_data = Vec::new();
        if use_tls {
            let _ = tokio_rustls::rustls::crypto::ring::default_provider().install_default();
            let verifier = Arc::new(InsecureCertVerifier);
            let client_config = ClientConfig::builder()
                .dangerous()
                .with_custom_certificate_verifier(verifier)
                .with_no_client_auth();
            let connector = TlsConnector::from(Arc::new(client_config));
            let server_name = ServerName::try_from("win-drop-pc.local")
                .unwrap_or_else(|_| ServerName::try_from("localhost").unwrap());

            let mut tls_stream = tokio::time::timeout(
                std::time::Duration::from_secs(5),
                connector.connect(server_name, stream),
            )
            .await
            .map_err(|_| anyhow::anyhow!("TLS handshake with {} timed out", target_addr))??;

            tls_stream.write_all(header_str.as_bytes()).await?;
            tls_stream.write_all(body).await?;
            tls_stream.flush().await?;

            let _ = tokio::time::timeout(
                std::time::Duration::from_secs(15),
                tls_stream.read_to_end(&mut resp_data),
            )
            .await;
        } else {
            let mut tcp_stream = stream;
            tcp_stream.write_all(header_str.as_bytes()).await?;
            tcp_stream.write_all(body).await?;
            tcp_stream.flush().await?;

            let _ = tokio::time::timeout(
                std::time::Duration::from_secs(15),
                tcp_stream.read_to_end(&mut resp_data),
            )
            .await;
        }

        Self::parse_http_response(&resp_data)
    }

    fn parse_http_response(data: &[u8]) -> anyhow::Result<(u16, Vec<u8>)> {
        if data.is_empty() {
            anyhow::bail!("Empty HTTP response received");
        }
        let header_end = data.windows(4).position(|w| w == b"\r\n\r\n");
        let (header_bytes, body_bytes) = match header_end {
            Some(idx) => (&data[..idx], &data[idx + 4..]),
            None => (data, &[][..]),
        };
        let header_str = String::from_utf8_lossy(header_bytes);
        let status_line = header_str.lines().next().unwrap_or("");
        let status_code: u16 = status_line
            .split_whitespace()
            .nth(1)
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        Ok((status_code, body_bytes.to_vec()))
    }
}

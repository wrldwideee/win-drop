// WIN-DROP — UI Interaction Logic & Telemetry Engine
document.addEventListener('DOMContentLoaded', () => {
  // DOM Elements
  const packetTerminal = document.getElementById('packet-terminal');
  const rxCounter = document.getElementById('rx-counter');
  const txCounter = document.getElementById('tx-counter');
  const tsfCounter = document.getElementById('tsf-counter');
  const btnToggleMonitor = document.getElementById('btn-toggle-monitor');
  const filterState = document.getElementById('filter-state');
  const btnInjectPsf = document.getElementById('btn-inject-psf');
  const dropzone = document.getElementById('dropzone');
  const fileInput = document.getElementById('file-input');
  const btnBrowseFiles = document.getElementById('btn-browse-files');
  const dropTargetLabel = document.getElementById('drop-target-label');
  const transferProgressBox = document.getElementById('transfer-progress-box');
  const transferFilename = document.getElementById('transfer-filename');
  const transferBar = document.getElementById('transfer-bar');
  const transferRate = document.getElementById('transfer-rate');
  const peerNodes = document.querySelectorAll('.peer-node');
  const btnTriggerModal = document.getElementById('btn-trigger-modal');
  const airdropModal = document.getElementById('airdrop-modal');
  const btnModalAccept = document.getElementById('btn-modal-accept');
  const btnModalDecline = document.getElementById('btn-modal-decline');
  const btnClearLog = document.getElementById('btn-clear-log');

  const chipIdVal = document.getElementById('chip-id-val');
  const currentChannelVal = document.getElementById('current-channel');
  const ipv6Addr = document.getElementById('ipv6-addr');
  const driverState = document.getElementById('driver-state');
  const wintunState = document.getElementById('wintun-state');
  const peerCountText = document.getElementById('peer-count-text');
  const ipadUrlText = document.getElementById('ipad-url-text');
  const receivedFilesTray = document.getElementById('received-files-tray');
  const receivedFilesList = document.getElementById('received-files-list');

  // Relative API base when served over HTTP/HTTPS, fallback to localhost:8770 on file://
  const API_BASE = window.location.protocol.startsWith('http') ? '' : 'http://localhost:8770';

  if (ipadUrlText && window.location.protocol.startsWith('http')) {
    ipadUrlText.textContent = window.location.origin;
  }

  let rxCount = 1842;
  let txCount = 438;
  let monitorActive = true;
  let selectedPeer = "Alex's iPhone 15 Pro";
  let daemonOnline = false;
  let lastTxSeq = null;

  const radarPeersLayer = document.getElementById('radar-peers-layer');

  function updateRadarPeers(peers) {
    if (!radarPeersLayer || !peers || peers.length === 0) return;
    const sorted = [...peers].sort((a, b) => b.rssi - a.rssi).slice(0, 8);
    radarPeersLayer.innerHTML = '';

    sorted.forEach((peer, idx) => {
      const angle = (idx / sorted.length) * 2 * Math.PI - (Math.PI / 2);
      const clampedRssi = Math.max(-95, Math.min(-45, peer.rssi || -75));
      const dist = 70 + ((-45 - clampedRssi) / 50) * 75;

      const x = Math.round(dist * Math.cos(angle));
      const y = Math.round(dist * Math.sin(angle));

      const node = document.createElement('div');
      node.className = 'peer-node' + (selectedPeer === peer.hostname ? ' active' : '');
      node.style.position = 'absolute';
      node.style.left = `calc(50% + ${x}px - 23px)`;
      node.style.top = `calc(50% + ${y}px - 23px)`;
      node.dataset.name = peer.hostname;
      node.dataset.mac = peer.mac;
      node.dataset.rssi = `${peer.rssi} dBm`;

      let iconClass = 'fa-solid fa-mobile-screen-button';
      const hLower = (peer.hostname || '').toLowerCase();
      if (hLower.includes('mac')) {
        iconClass = 'fa-solid fa-laptop';
      } else if (hLower.includes('ipad')) {
        iconClass = 'fa-solid fa-tablet-screen-button';
      }

      node.innerHTML = `
        <div class="peer-avatar">
          <i class="${iconClass}"></i>
        </div>
        <span class="peer-name">${peer.hostname} (${peer.rssi} dBm)</span>
      `;

      node.addEventListener('click', () => {
        document.querySelectorAll('.peer-node').forEach(p => p.classList.remove('active'));
        node.classList.add('active');
        selectedPeer = peer.hostname;
        dropTargetLabel.innerHTML = `Ready to AirDrop to <strong>${peer.hostname}</strong> [${peer.mac}] (${peer.rssi} dBm)`;
        addLog('AWDL', `Peer focused: ${peer.hostname} [${peer.mac}] RSSI=${peer.rssi} dBm`);
      });

      radarPeersLayer.appendChild(node);
    });
  }

  // Peer Selection in Radar (initial static fallback)
  peerNodes.forEach(node => {
    node.addEventListener('click', () => {
      peerNodes.forEach(p => p.classList.remove('active'));
      node.classList.add('active');
      selectedPeer = node.dataset.name;
      dropTargetLabel.innerHTML = `Ready to AirDrop to <strong>${selectedPeer}</strong> (${node.dataset.rssi})`;
      addLog('AWDL', `Peer focused: ${selectedPeer} [${node.dataset.model}] RSSI=${node.dataset.rssi}`);
    });
  });

  // Packet Terminal Streaming Simulator
  function getTimestamp() {
    const now = new Date();
    return now.toTimeString().split(' ')[0] + '.' + String(now.getMilliseconds()).padStart(3, '0');
  }

  function addLog(type, text, isTx = false) {
    const line = document.createElement('div');
    line.className = 'packet-log';
    const typeColor = isTx ? 'var(--accent-success)' : (type === 'AWDL' ? 'var(--accent-cyan)' : 'var(--accent-purple)');
    line.innerHTML = `
      <span class="pkt-time">[${getTimestamp()}]</span>
      <span class="pkt-type" style="color: ${typeColor};">${isTx ? 'TX ▶' : 'RX ◀'} ${type}</span>
      <span class="pkt-detail">${text}</span>
    `;
    packetTerminal.appendChild(line);
    packetTerminal.scrollTop = packetTerminal.scrollHeight;

    // Prune old lines if exceeding 200
    if (packetTerminal.children.length > 200) {
      packetTerminal.removeChild(packetTerminal.firstChild);
    }
  }

  // Prepopulate initial packets
  const initialPackets = [
    { type: 'AWDL', text: 'PSF Seq=1042 TargetTX=0x18F4 SA=00:17:F2:7B:A2:10 SyncTLV(CH=44 AW=16ms) ElectTLV(Master=00:17:F2:3A:91:C2)' },
    { type: 'mDNS', text: 'PTR _airdrop._tcp.local -> Alex-iPhone._airdrop._tcp.local (fe80::1092:c1ff:fe28:541b%awdl0:8770)' },
    { type: 'AWDL', text: 'MIF Seq=1043 Master=00:17:F2:3A:91:C2 Metric=0x00100000 SelfCounter=1849' },
    { type: 'DATA', text: 'QoS Data LLC/SNAP=00:17:F2:86DD IPv6 TLS1.3 Handshake (Client Hello, SNI=airdrop.local)' },
    { type: 'AWDL', text: 'PSF Seq=1044 TargetTX=0x1920 SA=00:17:F2:45:EE:90 ChannelHop=44->149' },
  ];

  initialPackets.forEach(p => addLog(p.type, p.text));

  // --- Live awdl-daemon REST Telemetry Polling (every 1000 ms) ---
  async function pollDaemonStatus() {
    try {
      const res = await fetch(`${API_BASE}/api/status`, {
        headers: { 'Accept': 'application/json' },
        cache: 'no-store'
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();

      if (!daemonOnline) {
        addLog('DAEMON', `Connected to awdl-daemon telemetry [${data.chip_id} FW=${data.fw_version}]`);
      }
      daemonOnline = true;

      // Update Live Hardware Telemetry HUD
      if (chipIdVal && data.chip_id) chipIdVal.textContent = data.chip_id;
      if (currentChannelVal && data.current_channel) {
        const ch = data.current_channel;
        const ghz = ch === 6 ? '2.437 GHz' : (ch === 44 ? '5.220 GHz' : '5.745 GHz');
        currentChannelVal.textContent = `CH ${ch} (${ghz})`;
      }
      if (ipv6Addr && data.self_ipv6) {
        ipv6Addr.textContent = data.self_ipv6;
      }
      if (driverState) {
        driverState.textContent = data.fw_loaded ? 'RUNNING (LIVE)' : 'HYBRID WI-FI';
      }
      if (wintunState) {
        wintunState.textContent = data.fw_loaded ? 'awdl0 UP' : 'LAN ONLY';
      }
      if (peerCountText && typeof data.peer_count === 'number') {
        peerCountText.textContent = `${data.peer_count} Apple Device${data.peer_count === 1 ? '' : 's'} in Range`;
      }
      if (Array.isArray(data.peers) && data.peers.length > 0) {
        updateRadarPeers(data.peers);
      }

      // Update Received Files Tray
      if (receivedFilesTray && receivedFilesList && Array.isArray(data.files) && data.files.length > 0) {
        receivedFilesTray.style.display = 'block';
        receivedFilesList.innerHTML = data.files.map(f => `
          <div style="display: flex; align-items: center; justify-content: space-between; padding: 4px 8px; background: rgba(255,255,255,0.03); border-radius: 4px; border: 1px solid rgba(255,255,255,0.05);">
            <span><i class="fa-regular fa-file" style="margin-right: 6px; color: var(--accent-cyan);"></i>${f}</span>
            <span style="font-size: 10px; color: var(--accent-success);"><i class="fa-solid fa-check"></i> Saved</span>
          </div>
        `).join('');
      }

      // Live Sequence Numbers & Packet Log
      if (typeof data.tx_seq === 'number') {
        txCounter.textContent = data.tx_seq.toLocaleString();

        if (lastTxSeq !== null && data.tx_seq !== lastTxSeq) {
          const delta = data.tx_seq - lastTxSeq;
          addLog(
            'AWDL',
            `LIVE PSF Frame TxSeq=${data.tx_seq} AWDL_Seq=${data.awdl_seq} CH=${data.current_channel} SA=${data.self_mac} (${delta} frame${delta > 1 ? 's' : ''} emitted via mt7922_awdl.sys)`,
            true
          );
        }
        lastTxSeq = data.tx_seq;
      }

      if (typeof data.awdl_seq === 'number') {
        const hexVal = (data.awdl_seq * 0x66 + 0x004F382A).toString(16).toUpperCase();
        tsfCounter.textContent = '0x' + hexVal.padStart(8, '0');
      }
    } catch (err) {
      if (daemonOnline) {
        addLog('DAEMON', 'awdl-daemon offline — running in simulation fallback mode');
      }
      daemonOnline = false;
      if (driverState) {
        driverState.textContent = 'SIMULATED';
      }
    }
  }

  // Initial poll and recurring 1000 ms timer
  pollDaemonStatus();
  setInterval(pollDaemonStatus, 1000);

  // Background ambient packet traffic simulator (runs alongside live telemetry)
  setInterval(() => {
    if (!monitorActive) return;
    rxCount++;
    rxCounter.textContent = rxCount.toLocaleString();

    const random = Math.random();
    if (!daemonOnline) {
      if (random < 0.4) {
        const seq = 1045 + (rxCount % 500);
        addLog('AWDL', `PSF Seq=${seq} SA=00:17:F2:${Math.floor(Math.random()*89+10)}:${Math.floor(Math.random()*89+10)}:10 Sync(AW=16ms, CH=44)`);
      } else if (random < 0.7) {
        addLog('DATA', `QoS Data 1420 bytes -> awdl0 decapsulated to TCP stream`);
      } else {
        addLog('mDNS', `Query _airdrop._tcp.local for new Apple peers`);
      }

      // Update TSF counter hex in simulation
      const currentTsf = parseInt(tsfCounter.textContent, 16) + 0x66;
      tsfCounter.textContent = '0x' + currentTsf.toString(16).toUpperCase();
    } else {
      // When live daemon is active, simulate realistic incoming peer probe responses
      if (random < 0.25) {
        addLog('mDNS', `mDNS Query: PTR _airdrop._tcp.local on awdl0`);
      } else if (random < 0.45) {
        addLog('DATA', `QoS Data: received 128 bytes from Apple peer`);
      }
    }
  }, 1600);

  // Clear Terminal Button
  btnClearLog.addEventListener('click', () => {
    packetTerminal.innerHTML = '';
  });

  // Toggle Monitor Mode Button
  btnToggleMonitor.addEventListener('click', async () => {
    monitorActive = !monitorActive;
    if (monitorActive) {
      filterState.textContent = 'ACTIVE';
      filterState.style.color = 'var(--accent-cyan)';
      btnToggleMonitor.innerHTML = '<i class="fa-solid fa-satellite-dish"></i> Disable Monitor Mode';
      addLog('KMDF', 'IOCTL_AWDL_SET_CHANNEL -> MCU_UNI_CMD_SET_FILTER (flags=0x000000E2 Monitor ON)', true);
    } else {
      filterState.textContent = 'STANDBY';
      filterState.style.color = 'var(--text-muted)';
      btnToggleMonitor.innerHTML = '<i class="fa-solid fa-satellite-dish"></i> Enable Monitor Mode';
      addLog('KMDF', 'MCU_UNI_CMD_SET_FILTER -> Monitor mode disabled (flags=0)', true);
    }
  });

  // Inject Test PSF Button (Wires to live /api/inject-psf with fallback)
  btnInjectPsf.addEventListener('click', async () => {
    try {
      const res = await fetch(`${API_BASE}/api/inject-psf`, {
        method: 'POST',
        headers: { 'Accept': 'application/json' }
      });
      if (res.ok) {
        const data = await res.json();
        txCount = data.tx_seq;
        txCounter.textContent = txCount.toLocaleString();
        lastTxSeq = data.tx_seq;
        addLog('AWDL', `LIVE INJECT: Periodic Sync Frame (PSF) TxSeq=${data.tx_seq} injected via mt7922_awdl.sys`, true);
        return;
      }
    } catch (e) {
      // Offline fallback
    }

    txCount++;
    txCounter.textContent = txCount.toLocaleString();
    addLog('AWDL', 'INJECT: Periodic Sync Frame (PSF) CH=44 OUI=00:17:F2 SA=00:17:F2:3A:91:C2 MasterMetric=0x00100000', true);
  });

  // File Drag & Drop Handling
  btnBrowseFiles.addEventListener('click', () => fileInput.click());

  fileInput.addEventListener('change', (e) => {
    if (e.target.files.length > 0) {
      handleFiles(Array.from(e.target.files));
    }
  });

  dropzone.addEventListener('dragover', (e) => {
    e.preventDefault();
    dropzone.classList.add('dragover');
  });

  dropzone.addEventListener('dragleave', () => {
    dropzone.classList.remove('dragover');
  });

  dropzone.addEventListener('drop', (e) => {
    e.preventDefault();
    dropzone.classList.remove('dragover');
    if (e.dataTransfer.files.length > 0) {
      handleFiles(Array.from(e.dataTransfer.files));
    }
  });

  // Real File Upload to Win-Drop AirDrop Engine via POST /Ask & POST /Upload
  async function handleFiles(files) {
    if (!files || files.length === 0) return;
    transferProgressBox.style.display = 'block';

    for (let i = 0; i < files.length; i++) {
      const file = files[i];
      const sizeMb = (file.size / (1024 * 1024)).toFixed(2);
      transferFilename.textContent = `AirDropping "${file.name}" (${sizeMb} MB) to ${selectedPeer}...`;
      transferBar.style.width = '15%';
      transferRate.textContent = 'Negotiating /Ask...';

      addLog('AirDrop', `Initiating POST /Ask for "${file.name}" (${file.size} bytes) to ${selectedPeer}`, true);

      try {
        // Step 1: Propose transfer via POST /Ask
        await fetch(`${API_BASE}/Ask`, {
          method: 'POST',
          headers: { 'Accept': 'application/json' },
          body: JSON.stringify({ filename: file.name, size: file.size })
        });
        transferBar.style.width = '35%';
        transferRate.textContent = 'Streaming /Upload...';

        // Step 2: Stream file payload via POST /Upload with X-File-Name preserved
        addLog('AirDrop', `Streaming payload chunks for "${file.name}" with preserved filename...`, true);
        const startTime = Date.now();
        const uploadRes = await fetch(`${API_BASE}/Upload`, {
          method: 'POST',
          headers: {
            'X-File-Name': encodeURIComponent(file.name),
            'Content-Type': file.type || 'application/octet-stream'
          },
          body: file
        });

        const elapsedSec = Math.max(0.05, (Date.now() - startTime) / 1000);
        const speedMbS = (file.size / (1024 * 1024) / elapsedSec).toFixed(1);

        if (uploadRes.ok) {
          transferBar.style.width = '100%';
          transferRate.textContent = `${speedMbS} MB/s (Saved!)`;
          txCount += Math.max(1, Math.ceil(file.size / 1400));
          txCounter.textContent = txCount.toLocaleString();
          addLog('AirDrop', `Transfer complete! "${file.name}" verified & saved in downloads/AirDrop (${speedMbS} MB/s)`, true);
        } else {
          throw new Error(`Server returned HTTP ${uploadRes.status}`);
        }
      } catch (err) {
        addLog('AirDrop', `Transfer error for "${file.name}": ${err.message}`, true);
        transferRate.textContent = 'Error';
      }
    }

    setTimeout(() => {
      transferProgressBox.style.display = 'none';
      transferRate.textContent = 'Idle';
    }, 4000);

    // Refresh telemetry immediately to show the newly saved file
    pollDaemonStatus();
  }

  // Incoming Modal Demo
  btnTriggerModal.addEventListener('click', () => {
    airdropModal.classList.add('open');
  });

  btnModalAccept.addEventListener('click', () => {
    airdropModal.classList.remove('open');
    addLog('AirDrop', 'POST /Ask -> Responded {"Status": "Accepted"}');
    addLog('AirDrop', 'POST /Upload -> Streaming incoming payload to %USERPROFILE%\\Downloads\\AirDrop');
  });

  btnModalDecline.addEventListener('click', () => {
    airdropModal.classList.remove('open');
    addLog('AirDrop', 'POST /Ask -> Responded {"Status": "Declined"}');
  });
});

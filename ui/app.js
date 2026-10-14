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

  let rxCount = 1842;
  let txCount = 438;
  let monitorActive = true;
  let selectedPeer = "Alex's iPhone 15 Pro";

  // Peer Selection in Radar
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

  // Periodic random packet arrival
  setInterval(() => {
    if (!monitorActive) return;
    rxCount++;
    rxCounter.textContent = rxCount.toLocaleString();

    const random = Math.random();
    if (random < 0.4) {
      const seq = 1045 + (rxCount % 500);
      addLog('AWDL', `PSF Seq=${seq} SA=00:17:F2:${Math.floor(Math.random()*89+10)}:${Math.floor(Math.random()*89+10)}:10 Sync(AW=16ms, CH=44)`);
    } else if (random < 0.7) {
      addLog('DATA', `QoS Data 1420 bytes -> awdl0 decapsulated to TCP stream`);
    } else {
      addLog('mDNS', `Query _airdrop._tcp.local for new Apple peers`);
    }

    // Update TSF counter hex
    const currentTsf = parseInt(tsfCounter.textContent, 16) + 0x66;
    tsfCounter.textContent = '0x' + currentTsf.toString(16).toUpperCase();
  }, 1600);

  // Clear Terminal Button
  btnClearLog.addEventListener('click', () => {
    packetTerminal.innerHTML = '';
  });

  // Toggle Monitor Mode Button
  btnToggleMonitor.addEventListener('click', () => {
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

  // Inject Test PSF Button
  btnInjectPsf.addEventListener('click', () => {
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

  function handleFiles(files) {
    const file = files[0];
    const totalSizeMb = (files.reduce((acc, f) => acc + f.size, 0) / (1024 * 1024)).toFixed(1);
    const summary = files.length === 1 ? file.name : `${files.length} files (${totalSizeMb} MB)`;

    transferProgressBox.style.display = 'block';
    transferFilename.textContent = `Sending "${summary}" to ${selectedPeer}`;
    transferBar.style.width = '0%';

    addLog('AirDrop', `Initiating POST /Ask to ${selectedPeer} on [fe80::...]:8770`, true);

    let progress = 0;
    const interval = setInterval(() => {
      progress += Math.floor(Math.random() * 15) + 10;
      if (progress >= 100) {
        progress = 100;
        clearInterval(interval);
        transferBar.style.width = '100%';
        transferRate.textContent = 'Complete!';
        addLog('AirDrop', `Transfer complete! ${summary} accepted & saved by ${selectedPeer}`, true);

        setTimeout(() => {
          transferProgressBox.style.display = 'none';
          transferRate.textContent = '48.2 MB/s';
        }, 3000);
      } else {
        transferBar.style.width = progress + '%';
        const rate = (40 + Math.random() * 15).toFixed(1);
        transferRate.textContent = `${rate} MB/s`;
        txCount += 4;
        txCounter.textContent = txCount.toLocaleString();
        addLog('DATA', `QoS Data Chunk -> Encap AWDL Frame (${progress}% complete)`, true);
      }
    }, 200);
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

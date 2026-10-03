// PeerYeet client. Protocol: docs/signaling-protocol.md.
'use strict';

const CHUNK = 64 * 1024;             // DataChannel message size
const READ_BLOCK = 16 * CHUNK;       // how much of the file to read at once
const HIGH_WATER = 8 * 1024 * 1024;  // pause when the DataChannel buffers this much
const LOW_WATER = 1024 * 1024;
const WINDOW = 32 * 1024 * 1024;     // max bytes in flight past the receiver's last ack
const ACK_EVERY = 2 * 1024 * 1024;
const COLLAPSE_EVERY = 64 * 1024 * 1024;  // in-memory fallback: fold chunks into one Blob
const CONNECT_TIMEOUT_MS = 30000;
// STUN only. There is deliberately no TURN server: if the browsers can't reach
// each other directly, the transfer fails rather than being relayed.
const ICE_SERVERS = [{ urls: ['stun:stun.l.google.com:19302', 'stun:stun.cloudflare.com:3478'] }];

const $ = id => document.getElementById(id);

// ---- helpers ----------------------------------------------------------------

function wsURL() {
  const override = new URLSearchParams(location.search).get('ws');
  if (override) return override;
  // Local development: page from a plain static server, signaling on :9000.
  if (location.port && location.port !== '443') return `ws://${location.hostname}:9000/ws`;
  return `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
}

function normalizeCode(s) {
  if (s.includes('#')) s = s.slice(s.lastIndexOf('#') + 1);
  return s.toUpperCase().replace(/[^A-Z0-9]+/g, '-').replace(/^-+|-+$/g, '');
}

function fmtBytes(n) {
  const units = ['bytes', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1000 && i < units.length - 1) { n /= 1000; i++; }
  return i === 0 ? `${n} bytes` : `${n.toFixed(n < 10 ? 2 : n < 100 ? 1 : 0)} ${units[i]}`;
}

function hex(buf) {
  return Array.from(new Uint8Array(buf), b => b.toString(16).padStart(2, '0')).join('');
}

// Chained integrity hash: h' = SHA-256(h || SHA-256(chunk)).
async function chain(h, chunkDigest) {
  const both = new Uint8Array(64);
  both.set(new Uint8Array(h), 0);
  both.set(new Uint8Array(chunkDigest), 32);
  return crypto.subtle.digest('SHA-256', both);
}

function show(view) {
  for (const id of ['home', 'send', 'recv', 'error']) $(id).hidden = id !== view;
  if (view === 'home') $('flow').hidden = true;
  $('ad').hidden = view !== 'home';  // also shown once a transfer is done, see stage()
}

// ---- the connection picture ---------------------------------------------------

const FLOW_LABELS = {
  waiting: 'Waiting for the other computer to connect.',
  pairing: 'The server is introducing the two computers.',
  direct: 'The two computers are connected directly. The server is no longer involved.',
  sending: 'Data is going directly from one computer to the other.',
  done: 'Delivered directly.',
  failed: 'No direct connection was possible, so nothing was sent.',
};

function makeFlow(a, b, state) {
  const svg = $('flow-tpl').content.firstElementChild.cloneNode(true);
  svg.querySelector('.name-a').textContent = a;
  svg.querySelector('.name-b').textContent = b;
  setFlowState(svg, state);
  return svg;
}

function setFlowState(svg, state) {
  svg.dataset.state = state;
  svg.setAttribute('aria-label', FLOW_LABELS[state] || '');
}

// The live picture for this page's transfer. Data always flows left to right.
let flow = null;
function startFlow(a, b, state) {
  flow = makeFlow(a, b, state);
  $('flow').replaceChildren(flow);
  $('flow').hidden = false;
}
function stage(state) {
  if (flow) setFlowState(flow, state);
  if (state === 'done') $('ad').hidden = false;
}

function setStatus(el, text, detail, ok) {
  el.textContent = text;
  el.classList.toggle('ok', !!ok);
  if (detail) {
    const small = document.createElement('small');
    small.textContent = detail;
    el.appendChild(small);
  }
}

let failed = false;
let busyTransferring = false;

// noDirectPath keeps the connection picture up, showing the broken line.
function fail(title, html, noDirectPath) {
  if (failed) return;
  failed = true;
  busyTransferring = false;
  $('e-title').textContent = title;
  $('e-body').innerHTML = html || '';
  show('error');
  if (noDirectPath) stage('failed');
  else $('flow').hidden = true;
}

function failText(title, text) {
  const p = document.createElement('p');
  p.textContent = text;
  fail(title, p.outerHTML);
}

const NO_DIRECT = `
  <p class="onpurpose">This is on purpose.</p>
  <p>PeerYeet only moves data straight from one computer to the other. These two couldn't
     reach each other directly, so we stopped. We won't route your data through our
     servers instead, the way other services quietly do.</p>
  <p>This usually means one side is on a network that blocks direct connections:
     a strict office or school firewall, some VPNs, or some mobile carriers.</p>
  <p>Things to try:</p>
  <ul>
    <li>Put both computers on the same Wi-Fi network.</li>
    <li>Turn off any VPN on either side.</li>
    <li>Try a different network on one side.</li>
  </ul>`;

// Watches transfer progress and keeps a smoothed rate.
function fmtSecs(secs) {
  if (secs < 60) return `${secs.toFixed(secs < 10 ? 1 : 0)} s`;
  return `${Math.floor(secs / 60)} min ${Math.round(secs % 60)} s`;
}

// Drives one of the progress blocks: prefix is 's' (sender) or 'r' (receiver).
function meter(prefix, total) {
  const [fillEl, pctEl, bytesEl, rateEl] = ['fill', 'pct', 'bytes', 'rate'].map(n => $(`${prefix}-${n}`));
  const start = performance.now();
  let samples = [[start, 0]];
  let last = 0;
  let queued = false;
  function render() {
    queued = false;
    const frac = total ? last / total : 1;
    fillEl.style.transform = `scaleX(${frac})`;
    pctEl.textContent = `${Math.floor(frac * 100)}%`;
    bytesEl.textContent = `${fmtBytes(last)} of ${fmtBytes(total)}`;
  }
  const timer = setInterval(() => {
    const t = performance.now();
    samples.push([t, last]);
    samples = samples.filter(([st]) => t - st <= 3000);
    const [t0, b0] = samples[0];
    const rate = t > t0 ? (last - b0) / ((t - t0) / 1000) : 0;
    rateEl.textContent = rate > 0 ? `${fmtBytes(rate)}/s` : '';
  }, 500);
  return {
    update(bytes) {
      last = bytes;
      if (!queued) { queued = true; requestAnimationFrame(render); }
    },
    stop() { clearInterval(timer); rateEl.textContent = ''; render(); },
    // e.g. "37.0 MB in 9.2 s (4.02 MB/s average)"
    summary() {
      const secs = (performance.now() - start) / 1000;
      return `${fmtBytes(total)} in ${fmtSecs(secs)} (${fmtBytes(total / Math.max(secs, 0.001))}/s average)`;
    },
  };
}

// ---- WebRTC setup ---------------------------------------------------------------

// Negotiates a DataChannel over the signaling WebSocket. The initiator (sender)
// creates the channel and the offer; the other side answers.
function startRTC(ws, initiator, { onOpen, onLost }) {
  const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });
  let opened = false;
  let remoteSet = false;
  const pendingCandidates = [];
  const sig = obj => { if (ws.readyState === WebSocket.OPEN) ws.send('S' + JSON.stringify(obj)); };

  const timer = setTimeout(() => { if (!opened) noDirect(); }, CONNECT_TIMEOUT_MS);
  function noDirect() {
    clearTimeout(timer);
    pc.close();
    fail('No direct path. Nothing was sent.', NO_DIRECT, true);
  }

  pc.onicecandidate = e => { if (e.candidate) sig({ c: e.candidate }); };
  pc.onconnectionstatechange = () => {
    if (pc.connectionState === 'failed') opened ? onLost() : noDirect();
  };

  function setup(dc) {
    dc.binaryType = 'arraybuffer';
    dc.onopen = () => {
      opened = true;
      clearTimeout(timer);
      onOpen(dc, pc);
    };
    dc.onclose = () => { if (opened) onLost(); };
  }

  if (initiator) {
    setup(pc.createDataChannel('yeet', { ordered: true }));
    pc.createOffer()
      .then(offer => pc.setLocalDescription(offer))
      .then(() => sig({ sdp: pc.localDescription }))
      .catch(err => failText('Connection setup failed', String(err)));
  } else {
    pc.ondatachannel = e => setup(e.channel);
  }

  return {
    async signal(msg) {
      try {
        if (msg.sdp) {
          await pc.setRemoteDescription(msg.sdp);
          remoteSet = true;
          while (pendingCandidates.length) await pc.addIceCandidate(pendingCandidates.shift());
          if (msg.sdp.type === 'offer') {
            await pc.setLocalDescription(await pc.createAnswer());
            sig({ sdp: pc.localDescription });
          }
        } else if (msg.c) {
          if (remoteSet) await pc.addIceCandidate(msg.c);
          else pendingCandidates.push(msg.c);
        }
      } catch (err) {
        failText('Connection setup failed', String(err));
      }
    },
    close() { clearTimeout(timer); pc.close(); },
  };
}

// Describes the network path WebRTC actually picked, for the status line.
async function describePath(pc) {
  try {
    const stats = await pc.getStats();
    let pair;
    stats.forEach(s => {
      if (s.type === 'transport' && s.selectedCandidatePairId) pair = stats.get(s.selectedCandidatePairId);
    });
    if (!pair) stats.forEach(s => { if (s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded') pair = s; });
    if (!pair) return '';
    const types = [stats.get(pair.localCandidateId), stats.get(pair.remoteCandidateId)]
      .map(c => c && c.candidateType);
    if (types.every(t => t === 'host')) return 'Same local network.';
    return 'Across the internet, through NAT.';
  } catch {
    return '';
  }
}

function openSignaling(onMessage) {
  const ws = new WebSocket(wsURL());
  ws.onmessage = e => onMessage(e.data[0], e.data.slice(1));
  return ws;
}

// ---- sending ----------------------------------------------------------------

function startSend(item) {
  show('send');
  startFlow('This computer', 'Receiver', 'waiting');
  $('s-name').textContent = item.name;
  $('s-size').textContent = fmtBytes(item.size);

  let rtc;
  let connected = false;
  const ws = openSignaling((type, body) => {
    switch (type) {
      case 'C': {
        $('s-code').textContent = body;
        $('s-host').textContent = location.host;
        const link = `${location.origin}${location.pathname}#${body}`;
        $('s-code').onclick = () => navigator.clipboard.writeText(body).then(() => {
          $('code-copied').textContent = 'Code copied.';
        });
        if (navigator.share) {
          $('share-link').hidden = false;
          $('share-link').onclick = () => navigator.share({ title: 'PeerYeet', text: `PeerYeet code: ${body}`, url: link }).catch(() => {});
        }
        const qr = qrcode(0, 'M');
        qr.addData(link);
        qr.make();
        $('qr').innerHTML = qr.createSvgTag({ cellSize: 4, margin: 2, scalable: true });
        $('copy-link').onclick = () => navigator.clipboard.writeText(link).then(() => {
          $('copy-link').textContent = 'Copied';
        });
        $('s-share').hidden = false;
        setStatus($('s-status'), 'Waiting for the receiver…');
        break;
      }
      case 'P':
        setStatus($('s-status'), 'Receiver connected. Looking for a direct path…',
          "If there isn't one, this stops here rather than routing through our server.");
        $('s-share').hidden = true;
        stage('pairing');
        rtc = startRTC(ws, true, { onOpen: (dc, pc) => { connected = true; onDirect(dc, pc); }, onLost });
        break;
      case 'S':
        if (rtc) rtc.signal(JSON.parse(body));
        break;
      case 'X':
        // Once negotiation has started, the peer closing its signaling socket is
        // normal (its DataChannel may open before ours); ICE failure or the
        // connect timeout covers a peer that really left.
        if (!rtc) failText('The receiver left', 'The receiving computer disconnected before the transfer started.');
        break;
      case 'E':
        if (!connected) failText('Transfer stopped', body);
        break;
    }
  });
  ws.onopen = () => ws.send('C');
  ws.onclose = () => { if (!connected) failText("Can't reach PeerYeet", 'The connection to the PeerYeet server was lost before the two computers connected.'); };

  let finished = false;
  function onLost() {
    if (!finished) fail('Connection lost', '<p>The direct connection to the receiver dropped before the transfer finished.</p>', true);
  }

  function onDirect(dc, pc) {
    ws.close();  // the server's job is done
    $('s-share').hidden = true;
    setStatus($('s-status'), 'Direct connection established.', '', true);
    describePath(pc).then(text => { $('s-path').textContent = text; });
    stage('direct');
    dc.send(JSON.stringify({ t: 'meta', kind: item.kind, name: item.name, size: item.size, mime: item.mime }));

    let acked = 0;
    let wake = null;
    const poke = () => { if (wake) { const w = wake; wake = null; w(); } };
    dc.bufferedAmountLowThreshold = LOW_WATER;
    dc.addEventListener('bufferedamountlow', poke);
    dc.addEventListener('close', poke);

    let m;
    dc.onmessage = e => {
      const msg = JSON.parse(e.data);
      if (msg.t === 'accept') {
        setStatus($('s-status'), 'Sending directly to the receiver…', 'Delivered bytes are confirmed by the receiver.', true);
        $('s-transfer').hidden = false;
        m = meter('s', item.size);
        stage('sending');
        m.update(0);
        busyTransferring = true;
        sendData().catch(err => { if (!finished) failText('Transfer failed', String(err)); });
      } else if (msg.t === 'decline') {
        finished = true;
        setStatus($('s-status'), 'The receiver declined.');
        $('s-keep').hidden = true;
        pc.close();
      } else if (msg.t === 'ack') {
        acked = msg.n;
        m.update(acked);
        poke();
      } else if (msg.t === 'done') {
        finished = true;
        busyTransferring = false;
        m.update(item.size);
        m.stop();
        $('s-keep').hidden = true;
        if (msg.ok) {
          setStatus($('s-status'), 'Delivered and verified.', m.summary(), true);
          stage('done');
        }
        else failText('Integrity check failed', 'The data arrived, but the receiver\'s checksum didn\'t match. Please try again.');
        setTimeout(() => pc.close(), 1000);
      }
    };

    async function sendData() {
      const blob = item.blob;
      const read = at => (at < blob.size ? blob.slice(at, at + READ_BLOCK).arrayBuffer() : null);
      let off = 0;
      let next = read(0);
      // Hashing runs alongside sending rather than in its way.
      let hash = Promise.resolve(new Uint8Array(32).buffer);
      while (next) {
        const buf = await next;
        next = read(off + buf.byteLength);  // read ahead while this block goes out
        const chunks = [];
        for (let i = 0; i < buf.byteLength; i += CHUNK) {
          chunks.push(new Uint8Array(buf, i, Math.min(CHUNK, buf.byteLength - i)));
        }
        hash = hash.then(async h => {
          for (const c of chunks) h = await chain(h, await crypto.subtle.digest('SHA-256', c));
          return h;
        });
        for (const c of chunks) {
          while (dc.readyState === 'open' && (dc.bufferedAmount > HIGH_WATER || off - acked > WINDOW)) {
            await new Promise(r => { wake = r; });
          }
          if (dc.readyState !== 'open') return;
          dc.send(c);
          off += c.byteLength;
        }
      }
      dc.send(JSON.stringify({ t: 'end', hash: hex(await hash) }));
    }
  }
}

// ---- receiving --------------------------------------------------------------

function startReceive(code) {
  show('recv');
  startFlow('Sender', 'This computer', 'pairing');
  setStatus($('r-status'), `Looking for ${code}…`);

  let rtc;
  let connected = false;
  const ws = openSignaling((type, body) => {
    switch (type) {
      case 'J':
        setStatus($('r-status'), 'Found the sender. Looking for a direct path…',
          "If there isn't one, this stops here rather than routing through our server.");
        rtc = startRTC(ws, false, { onOpen: (dc, pc) => { connected = true; onDirect(dc, pc); }, onLost });
        break;
      case 'S':
        if (rtc) rtc.signal(JSON.parse(body));
        break;
      case 'X':
        if (!rtc) failText('The sender left', 'The sending computer closed the page before the transfer started.');
        break;
      case 'E':
        if (!connected) failText("Couldn't connect", body);
        break;
    }
  });
  ws.onopen = () => ws.send('J' + code);
  ws.onclose = () => { if (!connected) failText("Can't reach PeerYeet", 'The connection to the PeerYeet server was lost before the two computers connected.'); };

  let finished = false;
  function onLost() {
    if (!finished) fail('Connection lost', '<p>The direct connection to the sender dropped before the transfer finished.</p>', true);
  }

  function onDirect(dc, pc) {
    ws.close();
    setStatus($('r-status'), 'Connected directly to the sender.', '', true);
    describePath(pc).then(text => { $('r-path').textContent = text; });
    stage('direct');

    let meta = null;
    let sink = null;       // FileSystemWritableFileStream, when the browser has one
    let parts = null;      // in-memory fallback
    let partsBytes = 0;
    let written = 0;
    let lastAck = 0;
    let h = new Uint8Array(32).buffer;
    let endHash = null;
    let m;
    const queue = [];
    let draining = false;

    dc.onmessage = e => {
      if (typeof e.data !== 'string') {
        if (!parts && !sink) return;  // data before accept: ignore
        queue.push(e.data);
        if (!draining) drain();
        return;
      }
      const msg = JSON.parse(e.data);
      if (msg.t === 'meta') offer(msg);
      else if (msg.t === 'end') {
        endHash = msg.hash;
        if (!draining) finish();
      }
    };

    function offer(msg) {
      meta = msg;
      $('r-name').textContent = msg.kind === 'text' ? 'Text' : msg.name;
      $('r-size').textContent = fmtBytes(msg.size);
      $('r-memwarn').hidden = !(msg.kind === 'file' && !window.showSaveFilePicker && msg.size > 1e9);
      $('r-offer').hidden = false;
      setStatus($('r-status'), `Connected directly to the sender. They want to send you ${msg.kind === 'text' ? 'some text' : 'a file'}.`, '', true);
    }

    $('accept').onclick = async () => {
      if (meta.kind === 'file' && window.showSaveFilePicker) {
        try {
          const handle = await window.showSaveFilePicker({ suggestedName: meta.name });
          sink = await handle.createWritable();
        } catch (err) {
          if (err.name === 'AbortError') return;  // they closed the picker; let them choose again
          sink = null;
        }
      }
      if (!sink) parts = [];
      $('r-offer').hidden = true;
      $('r-transfer').hidden = false;
      setStatus($('r-status'), 'Receiving directly from the sender…', '', true);
      m = meter('r', meta.size);
      stage('sending');
      m.update(0);
      busyTransferring = true;
      dc.send(JSON.stringify({ t: 'accept' }));
    };

    $('decline').onclick = () => {
      finished = true;
      dc.send(JSON.stringify({ t: 'decline' }));
      $('r-offer').hidden = true;
      setStatus($('r-status'), 'Declined. Nothing was received.');
      setTimeout(() => pc.close(), 500);
    };

    async function drain() {
      draining = true;
      try {
        while (queue.length) {
          const buf = queue.shift();
          if (written + buf.byteLength > meta.size) throw new Error('Sender sent more data than announced.');
          const digest = crypto.subtle.digest('SHA-256', buf);  // overlaps the write
          if (sink) {
            await sink.write(buf);
          } else {
            parts.push(buf);
            partsBytes += buf.byteLength;
            if (partsBytes >= COLLAPSE_EVERY) {  // lets the browser move data out of the JS heap
              parts = [new Blob(parts)];
              partsBytes = 0;
            }
          }
          h = await chain(h, await digest);
          written += buf.byteLength;
          m.update(written);
          if (written - lastAck >= ACK_EVERY || written === meta.size) {
            dc.send(JSON.stringify({ t: 'ack', n: written }));
            lastAck = written;
          }
        }
      } catch (err) {
        if (sink) sink.abort().catch(() => {});
        failText('Transfer failed', String(err.message || err));
        pc.close();
        return;
      } finally {
        draining = false;
      }
      if (endHash !== null) finish();
    }

    async function finish() {
      if (finished || written !== meta.size || (!sink && !parts)) return;
      finished = true;
      busyTransferring = false;
      m.stop();
      const ok = hex(h) === endHash;
      dc.send(JSON.stringify({ t: 'done', ok }));
      if (!ok) {
        if (sink) await sink.abort().catch(() => {});
        failText('Integrity check failed', 'The data arrived but didn\'t match what was sent, so it was discarded. Please try again.');
        return;
      }

      if (meta.kind === 'text') {
        const text = new TextDecoder().decode(await new Blob(parts).arrayBuffer());
        $('r-textbox').value = text;
        $('r-transfer').hidden = true;
        $('r-text').hidden = false;
        setStatus($('r-status'), 'Text received and verified.', m.summary(), true);
        stage('done');
        return;
      }

      if (sink) {
        await sink.close();
        setStatus($('r-status'), `Saved ${meta.name}. Verified.`, m.summary(), true);
        stage('done');
      } else {
        const url = URL.createObjectURL(new Blob(parts, { type: meta.mime || 'application/octet-stream' }));
        parts = null;
        const a = $('r-save');
        a.href = url;
        a.download = meta.name;
        a.textContent = `Save ${meta.name}`;
        a.hidden = false;
        a.click();
        setStatus($('r-status'), `Received ${meta.name}. Verified.`, m.summary(), true);
        stage('done');
      }
    }
  }
}

// ---- start page -------------------------------------------------------------

function sendFile(file) {
  startSend({ kind: 'file', name: file.name || 'pasted-file', size: file.size, mime: file.type, blob: file });
}

$('file').onchange = e => { if (e.target.files[0]) sendFile(e.target.files[0]); };
$('drop').onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('file').click(); } };

document.addEventListener('dragover', e => {
  if ($('home').hidden) return;
  e.preventDefault();
  document.body.classList.add('dragging');
});
document.addEventListener('dragleave', e => {
  if (!e.relatedTarget) document.body.classList.remove('dragging');
});
document.addEventListener('drop', e => {
  if ($('home').hidden) return;
  e.preventDefault();
  document.body.classList.remove('dragging');
  const file = e.dataTransfer.files[0];
  if (file) sendFile(file);
});

// Pasting a file (e.g. a screenshot) anywhere on the start page sends it.
// Pasted text goes into the text box so it can be checked before sending.
document.addEventListener('paste', e => {
  if ($('home').hidden) return;
  const file = e.clipboardData.files[0];
  if (file) {
    e.preventDefault();
    sendFile(file);
  } else if (document.activeElement !== $('text') && document.activeElement !== $('code')) {
    e.preventDefault();
    showTextPanel();
    $('text').value = e.clipboardData.getData('text');
    $('send-text').disabled = !$('text').value;
  }
});

function showTextPanel() {
  $('show-text').hidden = true;
  $('text-panel').hidden = false;
  $('text').focus();
}
$('show-text').onclick = showTextPanel;

// The two "How it works" pictures.
for (const el of document.querySelectorAll('.mini')) {
  el.appendChild(makeFlow(el.dataset.a, el.dataset.b, el.dataset.state));
}

$('text').oninput = () => { $('send-text').disabled = !$('text').value; };
$('send-text').onclick = () => {
  const bytes = new TextEncoder().encode($('text').value);
  startSend({ kind: 'text', name: 'Text', size: bytes.length, mime: 'text/plain', blob: new Blob([bytes]) });
};

$('join').onsubmit = e => {
  e.preventDefault();
  const code = normalizeCode($('code').value);
  if (code) startReceive(code);
};

$('copy-text').onclick = () => navigator.clipboard.writeText($('r-textbox').value).then(() => {
  $('copy-text').textContent = 'Copied';
});

$('restart').onclick = () => { location.href = location.pathname; };

window.addEventListener('beforeunload', e => {
  if (busyTransferring) e.preventDefault();
});

// Hit counter: one anonymous ping per page load (production only).
if (!location.port && navigator.sendBeacon) navigator.sendBeacon('/hit');

if (!window.RTCPeerConnection || !window.crypto || !crypto.subtle) {
  fail('This browser can\'t do direct transfers',
    '<p>PeerYeet needs WebRTC and the Web Crypto API. Use a current version of Chrome, Edge, Firefox, or Safari, over https.</p>');
} else if (location.hash.length > 1) {
  // Opened from a link or QR code.
  const code = normalizeCode(location.hash);
  history.replaceState(null, '', location.pathname + location.search);
  startReceive(code);
}

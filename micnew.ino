// ============================================================
// PCB Cupid — Wireless Studio Mic using Glyph C6
// ============================================================
// Turns a Glyph C6, 2CH MEMS Mic, and Micro SD Card module
// into a portable stereo recorder with its own WiFi hotspot
// and web-based control panel with audio preview.
//
// Dependencies (install via Arduino Library Manager):
//   - AudioTools by Phil Schatzmann
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <AudioTools.h>
#include <esp_task_wdt.h>
#include <cmath>
#include "config.h"

// ============================================================
// GLOBALS
// ============================================================

WebServer server(WEB_PORT);

// I2S captures at 32-bit (matches ICS-43434 hardware), WAV stored at 16-bit
AudioInfo captureInfo(SAMPLE_RATE, NUM_CHANNELS, I2S_BITS_PER_SAMPLE);
AudioInfo wavInfo(SAMPLE_RATE, NUM_CHANNELS, WAV_BITS_PER_SAMPLE);
I2SStream i2sStream;

bool isRecording       = false;
unsigned long recordStartMs = 0;
unsigned long samplesWritten = 0;     // per channel
File currentFile;
String currentFileName;

// ---- File counter for unique names ----
int fileCounter = 0;

// ---- Button debounce ----
unsigned long lastBtnCheck = 0;
bool lastBtnState         = HIGH;  // unpressed (pullup)
bool btnPressed           = false;

// ---- Audio Processing Variables ----
float envelope = 0.0f;
float noiseFloor = 0.001f;
float gainReduction = 1.0f;

// ---- File list cache ----
unsigned long lastFileListUpdate = 0;
String cachedFileList = "[]";

// ---- Storage limits ----
#define MIN_FREE_MB       100      // auto-stop when less than this free
#define STORAGE_CHECK_MS  5000     // check free space every 5s while recording
#define MAX_RECORDINGS    200      // max files to track in the list

// ============================================================
// SD CARD
// ============================================================

bool initSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  if (!SD.begin(SD_CS, SPI, 20000000)) {
    Serial.println("[SD] Card init failed — is the card inserted?");
    return false;
  }

  uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("[SD] No card detected");
    return false;
  }

  Serial.print("[SD] Card ready — ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  // Find next recording number by scanning existing files
  File root = SD.open("/");
  if (root) {
    while (true) {
      File f = root.openNextFile();
      if (!f) break;
      String name = f.name();
      if (name.startsWith("/recording_") && name.endsWith(".wav")) {
        int n = name.substring(11, name.length() - 4).toInt();
        if (n >= fileCounter) fileCounter = n + 1;
      }
      f.close();
    }
    root.close();
  }
  Serial.printf("[SD] Next recording will be #%d\n", fileCounter);
  return true;
}

// Sort recordings by sequence number
struct RecordingInfo {
  String name;
  size_t size;
  float duration;
  int sequence;
};

RecordingInfo gRecordings[MAX_RECORDINGS];  // global — not on stack

int compareRecordings(const void* a, const void* b) {
  RecordingInfo* ra = (RecordingInfo*)a;
  RecordingInfo* rb = (RecordingInfo*)b;
  // Newest first (higher sequence number = newer)
  return rb->sequence - ra->sequence;
}

// Extract sequence number from filename "recording_XXX.wav"
int extractSequence(String name) {
  // Remove leading slash if present
  if (name.startsWith("/")) {
    name = name.substring(1);
  }
  // Find the number between "recording_" and ".wav"
  int start = name.indexOf("recording_");
  if (start == -1) return 0;
  start += 10; // length of "recording_"
  int end = name.indexOf(".wav", start);
  if (end == -1) return 0;
  String numStr = name.substring(start, end);
  return numStr.toInt();
}

// List recordings on the SD card, returns JSON array fragment
String listRecordingsJSON() {
  // Cache file list for 2 seconds to reduce SD card reads
  unsigned long now = millis();
  if (now - lastFileListUpdate < 2000 && cachedFileList != "[]") {
    return cachedFileList;
  }
  
  String json = "[";
  File root = SD.open("/");
  if (!root) {
    json += "]";
    cachedFileList = json;
    lastFileListUpdate = now;
    return json;
  }
  
  // Collect all recordings into global array (not on stack)
  int count = 0;

  while (true) {
    File f = root.openNextFile();
    if (!f) break;

    String name = f.name();
    if (name.endsWith(".wav") && count < MAX_RECORDINGS) {
      gRecordings[count].name = name;
      gRecordings[count].size = f.size();
      size_t dataSize = (f.size() > 44) ? (f.size() - 44) : 0;
      gRecordings[count].duration = (float)dataSize / (SAMPLE_RATE * NUM_CHANNELS * (WAV_BITS_PER_SAMPLE / 8));
      gRecordings[count].sequence = extractSequence(name);
      count++;
    }
    f.close();
  }
  root.close();

  // Sort by sequence number (newest first)
  if (count > 1) {
    qsort(gRecordings, count, sizeof(RecordingInfo), compareRecordings);
  }

  // Build JSON
  bool first = true;
  for (int i = 0; i < count; i++) {
    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"name\":\"" + gRecordings[i].name + "\",";
    json += "\"size\":" + String(gRecordings[i].size) + ",";
    json += "\"duration\":" + String(gRecordings[i].duration, 1) + ",";
    json += "\"number\":" + String(gRecordings[i].sequence);
    json += "}";
  }
  
  json += "]";
  cachedFileList = json;
  lastFileListUpdate = now;
  return json;
}

// Human-readable file size
String formatSize(size_t bytes) {
  if (bytes < 1024)        return String(bytes) + " B";
  if (bytes < 1048576)     return String(bytes / 1024.0, 1) + " KB";
  return String(bytes / 1048576.0, 1) + " MB";
}

// ============================================================
// WAV FILE HELPERS
// ============================================================

void writeWavHeader(File &f, uint32_t dataBytes) {
  uint32_t sampleRate    = SAMPLE_RATE;
  uint16_t numChannels   = NUM_CHANNELS;
  uint16_t bitsPerSample = WAV_BITS_PER_SAMPLE;
  uint16_t bytesPerSample = bitsPerSample / 8;
  uint32_t byteRate      = sampleRate * numChannels * bytesPerSample;
  uint16_t blockAlign    = numChannels * bytesPerSample;
  uint32_t riffSize      = 36 + dataBytes;

  auto w32 = [&](uint32_t v) { f.write((uint8_t*)&v, 4); };
  auto w16 = [&](uint16_t v) { f.write((uint8_t*)&v, 2); };

  f.write((const uint8_t*)"RIFF", 4);  w32(riffSize);
  f.write((const uint8_t*)"WAVE", 4);

  f.write((const uint8_t*)"fmt ", 4);  w32(16);       // fmt chunk size
  w16(1);                               // PCM format
  w16(numChannels);
  w32(sampleRate);
  w32(byteRate);
  w16(blockAlign);
  w16(bitsPerSample);

  f.write((const uint8_t*)"data", 4);  w32(dataBytes);
}

// ============================================================
// AUDIO PROCESSING
// ============================================================

// Apply audio processing: preamp, noise gate, compressor
void processAudio(int16_t* samples, int count) {
  const float preamp = PREAMPLIFIER_GAIN;
  const float noiseThreshold = NOISE_GATE_THRESHOLD * 32768.0f;
  const float compThreshold = COMPRESSOR_THRESHOLD * 32768.0f;
  const float compRatio = COMPRESSOR_RATIO;
  const float maxReduction = MAX_GAIN_REDUCTION;
  
  for (int i = 0; i < count; i++) {
    float sample = samples[i];
    
    // 1. Pre-amplification
    sample *= preamp;
    
    // 2. Noise gate (reduces hiss when not speaking)
    float absSample = fabs(sample);
    if (absSample < noiseThreshold) {
      // Noise gate: smooth transition to zero
      float gate = absSample / noiseThreshold;
      sample *= gate * gate; // Quadratic fade for natural transition
    }
    
    // 3. Envelope follower for compression
    float alpha = 0.001f; // Smoothing factor
    envelope = alpha * absSample + (1 - alpha) * envelope;
    
    // 4. Compression
    if (envelope > compThreshold) {
      float excess = envelope - compThreshold;
      float compressedExcess = excess / compRatio;
      float targetEnvelope = compThreshold + compressedExcess;
      float targetGain = targetEnvelope / envelope;
      // Limit gain reduction
      if (targetGain < maxReduction) targetGain = maxReduction;
      
      // Apply gain smoothly
      static float currentGain = 1.0f;
      currentGain = currentGain * 0.95f + targetGain * 0.05f;
      sample *= currentGain;
    }
    
    // 5. Soft clipping (prevents harsh distortion)
    if (sample > 32767.0f) {
      // Soft clip using tanh approximation
      float over = (sample - 32767.0f) / 32767.0f;
      sample = 32767.0f - (over * over * 32767.0f * 0.5f);
    } else if (sample < -32767.0f) {
      float over = (-32767.0f - sample) / 32767.0f;
      sample = -32767.0f + (over * over * 32767.0f * 0.5f);
    }
    
    samples[i] = (int16_t)constrain(sample, -32767, 32767);
  }
}

// ============================================================
// RECORDING
// ============================================================

bool startRecording() {
  if (isRecording) return false;

  // Check free space before starting
  uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
  if (freeBytes < (uint64_t)MIN_FREE_MB * 1024ULL * 1024ULL) {
    Serial.printf("[REC] Low space! Free: %.1f MB, need %d MB\n",
                  freeBytes / (1024.0 * 1024.0), MIN_FREE_MB);
    return false;
  }

  // Find next free filename — never overwrite an existing recording
  do {
    currentFileName = "/recording_" + String(fileCounter++) + ".wav";
  } while (SD.exists(currentFileName));

  currentFile = SD.open(currentFileName, FILE_WRITE);
  if (!currentFile) {
    Serial.println("[REC] Cannot create file: " + currentFileName);
    return false;
  }

  // Placeholder header — will be re-written on stop
  writeWavHeader(currentFile, 0);

  // Configure I2S for capture at 32-bit (matches ICS-43434)
  auto cfg = i2sStream.defaultConfig(RX_MODE);
  cfg.copyFrom(captureInfo);
  cfg.i2s_format   = I2S_STD_FORMAT;
  cfg.pin_ws       = I2S_WS;
  cfg.pin_bck      = I2S_SCK;
  cfg.pin_data_rx  = I2S_SD;
  cfg.is_master    = true;
  cfg.use_apll     = false;

  if (!i2sStream.begin(cfg)) {
    Serial.println("[REC] I2S init failed");
    currentFile.close();
    SD.remove(currentFileName);
    return false;
  }

  isRecording    = true;
  recordStartMs  = millis();
  samplesWritten = 0;
  
  // Reset audio processing state
  envelope = 0.0f;
  gainReduction = 1.0f;

  pinMode(RECORD_LED, OUTPUT);
  digitalWrite(RECORD_LED, HIGH);

  Serial.println("[REC] Started → " + currentFileName);
  return true;
}

void stopRecording() {
  if (!isRecording) return;

  i2sStream.end();

  // Calculate actual data size and re-write header
  uint32_t dataBytes = samplesWritten * (WAV_BITS_PER_SAMPLE / 8);
  currentFile.seek(0);
  writeWavHeader(currentFile, dataBytes);
  currentFile.close();

  float duration = (float)samplesWritten / SAMPLE_RATE;

  Serial.println("[REC] Stopped → " + currentFileName);
  Serial.printf("[REC] %.1f sec, %s\n", duration, formatSize(dataBytes + 44).c_str());

  digitalWrite(RECORD_LED, LOW);
  isRecording = false;
  
  // Force file list refresh
  lastFileListUpdate = 0;
}

// Called every loop iteration — drains I2S DMA buffer into SD card.
void recordingLoop() {
  if (!isRecording) return;

  // Periodic free-space check — auto-stop before SD fills up
  static unsigned long lastSpaceCheck = 0;
  if (millis() - lastSpaceCheck > STORAGE_CHECK_MS) {
    lastSpaceCheck = millis();
    uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
    if (freeBytes < (uint64_t)MIN_FREE_MB * 1024ULL * 1024ULL) {
      Serial.printf("[REC] Space critical (%.1f MB free) — auto-stopping!\n",
                    freeBytes / (1024.0 * 1024.0));
      stopRecording();
      return;
    }
  }

  // Read 32-bit samples from I2S
  static int32_t rawBuf[CHUNK_SIZE / 4];  // 512 samples for 2 channels
  size_t bytesRead = i2sStream.readBytes((uint8_t*)rawBuf, CHUNK_SIZE);

  if (bytesRead > 0) {
    int numSamples = bytesRead / 4;  // 32-bit = 4 bytes per sample

    // Convert 32-bit → 16-bit and process audio
    static int16_t wavBuf[CHUNK_SIZE / 2];
    for (int i = 0; i < numSamples; i++) {
      // Extract 16-bit from 32-bit (ICS-43434 puts 24-bit in top of 32-bit)
      wavBuf[i] = (int16_t)(rawBuf[i] >> 16);
    }
    
    // Apply audio processing (preamp, noise gate, compression)
    processAudio(wavBuf, numSamples);

    size_t wavBytes = numSamples * 2;  // 16-bit = 2 bytes per sample
    currentFile.write((uint8_t*)wavBuf, wavBytes);
    samplesWritten += numSamples;
  }

  // Blink LED while recording (on/off every 500ms)
  if (samplesWritten % (SAMPLE_RATE / 2) < (SAMPLE_RATE / 4)) {
    digitalWrite(RECORD_LED, HIGH);
  } else {
    digitalWrite(RECORD_LED, LOW);
  }

  // Feed the watchdog
  esp_task_wdt_reset();
}

// ---- Onboard BOOT key as record/stop button (GPIO 9) ----
void handleButton() {
  unsigned long now = millis();
  if (now - lastBtnCheck < 30) return;   // debounce
  lastBtnCheck = now;

  bool state = digitalRead(RECORD_BTN);
  if (state == LOW && lastBtnState == HIGH) {
    // falling edge — button pressed
    if (isRecording) {
      stopRecording();
      Serial.println("[BTN] Recording stopped via button");
    } else {
      startRecording();
      Serial.println("[BTN] Recording started via button");
    }
  }
  lastBtnState = state;
}

// ============================================================
// WEB SERVER — HTML PAGE with Audio Preview
// ============================================================

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Glyph Studio Mic Pro</title>
<style>
  *, *::before, *::after { box-sizing:border-box; margin:0; padding:0; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    background: #0f0f0f; color: #e0e0e0; min-height: 100vh;
    display: flex; justify-content: center; padding: 20px;
  }
  .container { max-width: 600px; width: 100%; }
  h1 { font-size: 1.3rem; font-weight: 600; margin-bottom: 4px; color: #fff; }
  .subtitle { font-size: 0.8rem; color: #888; margin-bottom: 24px; }

  .status-bar {
    display: flex; align-items: center; gap: 10px; margin-bottom: 12px;
    padding: 14px 18px; border-radius: 12px; background: #1a1a1a;
  }
  .status-dot {
    width: 12px; height: 12px; border-radius: 50%; flex-shrink: 0;
    background: #444; transition: background 0.3s;
  }
  .status-dot.recording { background: #ff3b30; box-shadow: 0 0 8px #ff3b30; animation: pulse 1s infinite; }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.4} }
  .status-text { font-size: 0.95rem; flex: 1; }
  .status-time { font-size: 1.4rem; font-weight: 700; font-variant-numeric: tabular-nums; color: #fff; }

  /* Audio Level Meter - The colorful line you asked about */
  .level-meter {
    height: 4px; background: #1a1a1a; border-radius: 2px; margin-bottom: 20px; overflow: hidden;
  }
  .level-meter-fill {
    height: 100%; width: 0%; background: linear-gradient(90deg, #4CAF50, #FFEB3B, #ff3b30);
    transition: width 0.05s;
  }
  /* The colors show: Green = quiet, Yellow = medium, Red = loud (clipping warning) */

  .controls { display: flex; gap: 10px; margin-bottom: 24px; }
  button {
    flex: 1; padding: 14px 0; border: none; border-radius: 10px;
    font-size: 0.95rem; font-weight: 600; cursor: pointer; transition: all 0.2s;
  }
  button:active { transform: scale(0.97); }
  #btnRecord {
    background: #ff3b30; color: #fff;
  }
  #btnRecord.recording { background: #1a1a1a; color: #ff3b30; border: 1.5px solid #ff3b30; }
  #btnRecord:disabled { opacity: 0.3; pointer-events: none; }

  .section-title {
    font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.5px;
    color: #666; margin-bottom: 10px;
  }
  .file-list { list-style: none; }
  .file-item {
    display: flex; flex-wrap: wrap; align-items: center; gap: 8px;
    padding: 12px 14px; background: #1a1a1a; border-radius: 10px;
    margin-bottom: 6px;
  }
  .file-info { flex: 1; min-width: 140px; }
  .file-name { font-size: 0.85rem; color: #fff; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .file-meta { font-size: 0.7rem; color: #777; margin-top: 2px; }
  .file-actions {
    display: flex; gap: 6px; flex-wrap: wrap;
  }
  .btn-action {
    background: #2a2a2a; color: #e0e0e0; padding: 6px 12px; border-radius: 6px;
    border: none; font-size: 0.75rem; font-weight: 500; cursor: pointer;
    text-decoration: none; transition: background 0.2s;
  }
  .btn-action:hover { background: #3a3a3a; }
  .btn-download { background: #2d6ff7; color: #fff; }
  .btn-download:hover { background: #1a5ce5; }
  .btn-play { background: #4CAF50; color: #fff; }
  .btn-play:hover { background: #388E3C; }
  .btn-play.playing { background: #ff3b30; }
  .file-number {
    background: #2a2a2a; padding: 2px 8px; border-radius: 12px;
    font-size: 0.7rem; color: #888; font-weight: bold;
  }

  .empty-state {
    text-align: center; padding: 40px 20px; color: #555;
  }
  .empty-state .icon { font-size: 2rem; margin-bottom: 8px; }

  .footer {
    text-align: center; margin-top: 30px; font-size: 0.7rem; color: #444;
  }

  /* Storage bar */
  .storage-bar-outer {
    height: 6px; background: #1a1a1a; border-radius: 3px; margin-bottom: 6px; overflow: hidden;
  }
  .storage-bar-inner {
    height: 100%; border-radius: 3px; transition: width 1s;
    background: #4CAF50;
  }
  .storage-bar-inner.warning { background: #FF9800; }
  .storage-bar-inner.danger  { background: #ff3b30; }
  .storage-label {
    font-size: 0.7rem; color: #777; margin-bottom: 20px;
    display: flex; justify-content: space-between;
  }
  .storage-label.low { color: #ff3b30; font-weight: 600; }

  /* Inline rename input */
  .rename-input {
    background: #2a2a2a; border: 1px solid #555; color: #fff;
    padding: 4px 8px; border-radius: 6px; font-size: 0.85rem;
    width: 140px; outline: none;
  }
  .rename-input:focus { border-color: #2d6ff7; }

  .btn-rename { background: #8e44ad; color: #fff; }
  .btn-rename:hover { background: #7d3c98; }
  .btn-delete { background: #c0392b; color: #fff; }
  .btn-delete:hover { background: #a93226; }

  .toast {
    position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%);
    background: #2d6ff7; color: #fff; padding: 10px 24px; border-radius: 20px;
    font-size: 0.85rem; opacity: 0; transition: opacity 0.3s; pointer-events: none; z-index: 10;
  }
  .toast.show { opacity: 1; }
</style>
</head>
<body>
<div class="container">

  <h1>🎙️ Glyph Studio Mic Pro</h1>
  <p class="subtitle">Wireless Stereo Recorder — PCB Cupid</p>

  <div class="status-bar">
    <div class="status-dot" id="statusDot"></div>
    <span class="status-text" id="statusText">Ready</span>
    <span class="status-time" id="statusTime">00:00</span>
  </div>
  <div class="level-meter">
    <div class="level-meter-fill" id="levelMeter"></div>
  </div>

  <div class="storage-bar-outer">
    <div class="storage-bar-inner" id="storageBar" style="width:0%"></div>
  </div>
  <div class="storage-label" id="storageLabel">
    <span>SD Card</span><span>-- GB free</span>
  </div>

  <div class="controls">
    <button id="btnRecord" onclick="toggleRecord()">Start Recording</button>
  </div>

  <p class="section-title">📁 Recordings on SD Card</p>
  <ul class="file-list" id="fileList"></ul>
  <div class="empty-state" id="emptyState">
    <div class="icon">🎙️</div>
    <p>No recordings yet. Press record to start.</p>
  </div>

  <p class="footer">PCB Cupid &bull; Glyph C6 &bull; G-Sense 2CH Mic</p>

</div>
<div class="toast" id="toast"></div>

<script>
  const btnRecord  = document.getElementById('btnRecord');
  const statusDot  = document.getElementById('statusDot');
  const statusText = document.getElementById('statusText');
  const statusTime = document.getElementById('statusTime');
  const levelMeter = document.getElementById('levelMeter');
  const fileList   = document.getElementById('fileList');
  const emptyState = document.getElementById('emptyState');
  const toast      = document.getElementById('toast');

  let currentAudio = null;
  let playingFile = null;

  function fmtTime(sec) {
    const m = Math.floor(sec / 60);
    const s = Math.floor(sec % 60);
    return String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');
  }

  function fmtSize(b) {
    if (b < 1024) return b + ' B';
    if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
    return (b/1048576).toFixed(1) + ' MB';
  }

  function toggleRecord() {
    const url = btnRecord.classList.contains('recording')
      ? '/api/record/stop'
      : '/api/record/start';
    fetch(url).then(r => r.text()).then(txt => {
      if (txt === 'OK') poll();
    });
  }

  function downloadFile(name) {
    window.location.href = '/download?file=' + encodeURIComponent(name);
  }

  function playAudio(name, button) {
    if (currentAudio) {
      currentAudio.pause();
      currentAudio = null;
      if (playingFile === name) {
        playingFile = null;
        button.textContent = '▶';
        button.classList.remove('playing');
        return;
      }
    }
    
    // Show loading state
    button.textContent = '⏳';
    button.disabled = true;
    
    fetch('/stream?file=' + encodeURIComponent(name))
      .then(r => r.blob())
      .then(blob => {
        const url = URL.createObjectURL(blob);
        currentAudio = new Audio(url);
        currentAudio.play();
        playingFile = name;
        button.textContent = '⏹';
        button.classList.add('playing');
        button.disabled = false;
        
        currentAudio.onended = () => {
          button.textContent = '▶';
          button.classList.remove('playing');
          playingFile = null;
          URL.revokeObjectURL(url);
        };
      })
      .catch(err => {
        showToast('Error playing audio');
        console.error(err);
        button.textContent = '▶';
        button.disabled = false;
      });
  }

  function showToast(msg) {
    toast.textContent = msg;
    toast.classList.add('show');
    setTimeout(() => toast.classList.remove('show'), 2000);
  }

  function deleteFile(name, btn) {
    if (!confirm('Delete ' + name.replace('/','') + '?')) return;
    btn.textContent = '...'; btn.disabled = true;
    fetch('/api/delete?file=' + encodeURIComponent(name))
      .then(r => r.text())
      .then(txt => {
        if (txt === 'OK') { showToast('Deleted'); poll(); }
        else { showToast('Delete failed'); btn.textContent = '🗑'; btn.disabled = false; }
      });
  }

  function renameFile(oldName, btn) {
    const li = btn.closest('.file-item');
    const nameDiv = li.querySelector('.file-name');
    const current = nameDiv.textContent.replace(/^#\d+\s*/, '').replace('.wav', '').trim();
    const input = document.createElement('input');
    input.type = 'text'; input.className = 'rename-input'; input.value = current;
    nameDiv.innerHTML = ''; nameDiv.appendChild(input);
    input.focus(); input.select();

    const finish = () => {
      const newName = input.value.trim();
      if (!newName || newName === current) { poll(); return; }
      fetch('/api/rename?file=' + encodeURIComponent(oldName) + '&new=' + encodeURIComponent(newName))
        .then(r => r.text())
        .then(txt => {
          if (txt === 'OK') { showToast('Renamed'); poll(); }
          else { showToast('Rename failed — name may already exist'); poll(); }
        });
    };
    input.addEventListener('blur', finish);
    input.addEventListener('keydown', e => { if (e.key === 'Enter') finish(); });
  }

  function poll() {
    fetch('/api/status')
      .then(r => r.json())
      .then(data => {
        // Recording state
        if (data.recording) {
          statusDot.classList.add('recording');
          statusText.textContent = '🔴 Recording...';
          statusTime.textContent = fmtTime(data.elapsed);
          btnRecord.textContent = '⏹ Stop Recording';
          btnRecord.classList.add('recording');
        } else {
          statusDot.classList.remove('recording');
          statusText.textContent = '✅ Ready';
          statusTime.textContent = '00:00';
          btnRecord.textContent = '🔴 Start Recording';
          btnRecord.classList.remove('recording');
        }

        // Level meter
        if (data.level !== undefined) {
          levelMeter.style.width = Math.min(data.level * 100, 100) + '%';
        }

        // Storage bar
        if (data.totalMB !== undefined) {
          const pct = data.totalMB > 0 ? (data.usedMB / data.totalMB * 100) : 0;
          const bar = document.getElementById('storageBar');
          const lbl = document.getElementById('storageLabel');
          bar.style.width = pct + '%';
          bar.className = 'storage-bar-inner' +
            (data.lowSpace ? ' danger' : (pct > 80 ? ' warning' : ''));
          lbl.innerHTML = '<span>SD: ' + data.usedMB + ' / ' + data.totalMB + ' MB</span>' +
            '<span class="' + (data.lowSpace ? 'low' : '') + '">' + data.freeMB + ' MB free</span>';
        }

        // File list — all recordings, newest first
        fileList.innerHTML = '';
        if (data.files.length === 0) {
          emptyState.style.display = 'block';
        } else {
          emptyState.style.display = 'none';
          data.files.forEach(f => {
            const li = document.createElement('li');
            li.className = 'file-item';
            const displayName = f.name.replace('/','').replace('recording_','').replace('.wav','');
            li.innerHTML =
              '<div class="file-info">' +
                '<div class="file-name">' +
                  '<span class="file-number">#' + f.number + '</span> ' + displayName +
                '</div>' +
                '<div class="file-meta">⏱ ' + fmtTime(f.duration) + ' · 💾 ' + fmtSize(f.size) + '</div>' +
              '</div>' +
              '<div class="file-actions">' +
                '<button class="btn-action btn-play" onclick="playAudio(\'' + f.name + '\', this)">▶</button>' +
                '<button class="btn-action btn-rename" onclick="renameFile(\'' + f.name + '\', this)">✏</button>' +
                '<button class="btn-action btn-download" onclick="downloadFile(\'' + f.name + '\')">⬇</button>' +
                '<button class="btn-action btn-delete" onclick="deleteFile(\'' + f.name + '\', this)">🗑</button>' +
              '</div>';
            fileList.appendChild(li);
          });
        }
      })
      .catch(err => console.error('Poll error:', err));
  }

  // Poll every second
  setInterval(poll, 1000);
  poll();
</script>
</body>
</html>
)rawliteral";

// ============================================================
// WEB SERVER — ROUTES
// ============================================================

void setupWebServer() {

  // ---- Main page ----
  server.on("/", []() {
    server.send(200, "text/html", PAGE_HTML);
  });

  // ---- API: Status (JSON) ----
  server.on("/api/status", []() {
    uint64_t totalBytes = SD.totalBytes();
    uint64_t usedBytes  = SD.usedBytes();
    uint64_t freeBytes  = totalBytes - usedBytes;

    String json = "{";
    json += "\"recording\":" + String(isRecording ? "true" : "false") + ",";
    json += "\"elapsed\":" + String(isRecording ? (millis() - recordStartMs) / 1000 : 0) + ",";
    float level = envelope / 32768.0f;
    json += "\"level\":" + String(level * 1.5f) + ",";
    json += "\"totalMB\":" + String((int)(totalBytes / (1024 * 1024))) + ",";
    json += "\"usedMB\":"  + String((int)(usedBytes  / (1024 * 1024))) + ",";
    json += "\"freeMB\":"  + String((int)(freeBytes  / (1024 * 1024))) + ",";
    json += "\"lowSpace\":" + String(freeBytes < (uint64_t)MIN_FREE_MB * 1024ULL * 1024ULL ? "true" : "false") + ",";
    json += "\"files\":" + listRecordingsJSON();
    json += "}";
    server.send(200, "application/json", json);
  });

  // ---- API: Start recording ----
  server.on("/api/record/start", []() {
    if (startRecording()) {
      server.send(200, "text/plain", "OK");
    } else {
      server.send(500, "text/plain", "SD card error");
    }
  });

  // ---- API: Stop recording ----
  server.on("/api/record/stop", []() {
    stopRecording();
    server.send(200, "text/plain", "OK");
  });

  // ---- API: Delete a recording ----
  server.on("/api/delete", []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing file");
      return;
    }
    String filename = server.arg("file");
    if (filename.indexOf("..") != -1) { server.send(403); return; }
    if (!filename.startsWith("/")) filename = "/" + filename;

    if (SD.remove(filename)) {
      lastFileListUpdate = 0;  // force list refresh
      Serial.println("[DEL] " + filename);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(500, "text/plain", "Delete failed");
    }
  });

  // ---- API: Rename a recording ----
  server.on("/api/rename", []() {
    if (!server.hasArg("file") || !server.hasArg("new")) {
      server.send(400, "text/plain", "Missing file or new name");
      return;
    }
    String oldName = server.arg("file");
    String newName = server.arg("new");
    if (oldName.indexOf("..") != -1 || newName.indexOf("..") != -1) {
      server.send(403); return;
    }
    if (!oldName.startsWith("/")) oldName = "/" + oldName;
    if (!newName.startsWith("/")) newName = "/" + newName;
    if (!newName.endsWith(".wav")) newName += ".wav";

    if (SD.rename(oldName, newName)) {
      lastFileListUpdate = 0;
      Serial.println("[REN] " + oldName + " → " + newName);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(500, "text/plain", "Rename failed — name may already exist");
    }
  });

  // ---- Stream audio for preview (optimized for speed) ----
  server.on("/stream", []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing file parameter");
      return;
    }

    String filename = server.arg("file");
    if (filename.indexOf("..") != -1) {
      server.send(403, "text/plain", "Forbidden");
      return;
    }
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }

    if (!SD.exists(filename)) {
      server.send(404, "text/plain", "File not found");
      return;
    }

    File audioFile = SD.open(filename, FILE_READ);
    if (!audioFile) {
      server.send(500, "text/plain", "Cannot open file");
      return;
    }

    size_t fileSize = audioFile.size();
    WiFiClient client = server.client();
    
    // Send headers for streaming
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: audio/wav");
    client.println("Content-Length: " + String(fileSize));
    client.println("Connection: close");
    client.println();

    // Stream the file with larger buffer for speed
    uint8_t buffer[2048]; // Increased buffer size
    size_t remaining = fileSize;
    while (remaining > 0 && client.connected()) {
      size_t toRead = (remaining > 2048) ? 2048 : remaining;
      size_t bytes = audioFile.read(buffer, toRead);
      if (bytes == 0) break;
      client.write(buffer, bytes);
      remaining -= bytes;
      esp_task_wdt_reset();
      yield();
    }
    client.flush();
    audioFile.close();
  });

  // ---- Download a file ----
  server.on("/download", []() {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing file parameter");
      return;
    }

    String filename = server.arg("file");
    if (filename.indexOf("..") != -1) {
      server.send(403, "text/plain", "Forbidden");
      return;
    }
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }

    if (!SD.exists(filename)) {
      server.send(404, "text/plain", "File not found");
      return;
    }

    File downloadFile = SD.open(filename, FILE_READ);
    if (!downloadFile) {
      server.send(500, "text/plain", "Cannot open file");
      return;
    }

    size_t fileSize = downloadFile.size();
    WiFiClient client = server.client();
    
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: audio/wav");
    client.println("Content-Disposition: attachment; filename=\"" + filename.substring(1) + "\"");
    client.println("Content-Length: " + String(fileSize));
    client.println("Connection: close");
    client.println();

    uint8_t buffer[2048];
    size_t remaining = fileSize;
    while (remaining > 0 && client.connected()) {
      size_t toRead = (remaining > 2048) ? 2048 : remaining;
      size_t bytes = downloadFile.read(buffer, toRead);
      if (bytes == 0) break;
      client.write(buffer, bytes);
      remaining -= bytes;
      esp_task_wdt_reset();
      yield();
    }
    client.flush();
    downloadFile.close();
  });

  // ---- 404 ----
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[WEB] Server started on http://" + WiFi.softAPIP().toString());
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("══════════════════════════════════════");
  Serial.println("  PCB Cupid — Wireless Studio Mic Pro");
  Serial.println("  Glyph C6 + G-Sense 2CH Mic + SD Card");
  Serial.println("══════════════════════════════════════");

  // ---- Watchdog: subscribe the main loop task ----
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  // ---- Memory ----
  Serial.printf("[BOOT] Free heap: %d KB\n", ESP.getFreeHeap() / 1024);

  // ---- SD Card ----
  Serial.print("[INIT] SD Card... ");
  if (!initSD()) {
    Serial.println("FATAL — check card and wiring, then reset.");
    while (1) { delay(1000); }
  }

  // ---- WiFi Access Point ----
  Serial.print("[INIT] WiFi AP... ");
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println(WiFi.softAPIP());

  // ---- Web Server ----
  setupWebServer();

  // ---- Onboard LED & Button ----
  pinMode(RECORD_LED, OUTPUT);
  digitalWrite(RECORD_LED, LOW);
  pinMode(RECORD_BTN, INPUT_PULLUP);

  Serial.println("[READY] Connect to WiFi: \"" AP_SSID "\"");
  Serial.println("[READY] Web panel → http://" + WiFi.softAPIP().toString());
  Serial.println("[READY] Or press the onboard BOOT button to record/stop");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  server.handleClient();
  recordingLoop();
  handleButton();
  delay(1);
  esp_task_wdt_reset();
}
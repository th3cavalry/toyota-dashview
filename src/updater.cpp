#include "updater.h"
#include <WebServer.h>
#include <Update.h>
#include "version.h"
#include "profile.h"

bool isUpdateInProgress = false;
int updateProgressPercent = 0;
char updateStatusMessage[64] = "";

static WebServer webServer(80);
static bool s_webServerStarted = false;

// HTML page for / (Dashboard & Device Info)
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>DashView Manager</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0e1017; color: #e1e7f0; margin: 0; padding: 20px; text-align: center; }
    .card { background: #161a23; border: 1px solid #232a3b; border-radius: 12px; max-width: 480px; margin: 20px auto; padding: 24px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); text-align: left; }
    h1 { color: #ffffff; font-size: 24px; margin-top: 0; display: flex; align-items: center; gap: 10px; }
    .badge { background: #e02424; color: #fff; font-size: 11px; padding: 3px 8px; border-radius: 6px; font-weight: bold; text-transform: uppercase; }
    .item { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #202636; font-size: 14px; }
    .label { color: #8a96a8; }
    .val { color: #ffffff; font-weight: 600; }
    .btn { display: block; width: 100%; box-sizing: border-box; background: #2563eb; color: #fff; text-decoration: none; padding: 14px; border-radius: 8px; font-weight: bold; font-size: 16px; margin-top: 20px; text-align: center; border: none; cursor: pointer; }
    .btn:hover { background: #1d4ed8; }
    .btn-update { background: #d97706; }
    .btn-update:hover { background: #b45309; }
  </style>
</head>
<body>
  <div class="card">
    <h1>DashView <span class="badge">Online</span></h1>
    <div class="item"><span class="label">Firmware Version</span><span class="val">APP_VER</span></div>
    <div class="item"><span class="label">Vehicle Profile</span><span class="val">VEHICLE_NAME</span></div>
    <div class="item"><span class="label">Hardware</span><span class="val">ESP32-S3 4.3" Touch</span></div>
    <div class="item"><span class="label">SavvyCAN Port</span><span class="val">1337</span></div>
    <div class="item"><span class="label">Signal Decoders</span><span class="val">SIG_COUNT active</span></div>
    
    <a href="/update" class="btn btn-update">Install Firmware Update</a>
  </div>
</body>
</html>
)rawliteral";

// HTML page for /update (Firmware Upload with Progress Bar)
static const char UPDATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>DashView Firmware Update</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0e1017; color: #e1e7f0; margin: 0; padding: 20px; text-align: center; }
    .card { background: #161a23; border: 1px solid #232a3b; border-radius: 12px; max-width: 480px; margin: 20px auto; padding: 24px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); text-align: left; }
    h1 { color: #ffffff; font-size: 22px; margin-top: 0; }
    p { color: #8a96a8; font-size: 14px; line-height: 1.5; }
    .dropzone { border: 2px dashed #374151; border-radius: 8px; padding: 28px; text-align: center; cursor: pointer; margin: 20px 0; background: #131720; }
    .dropzone:hover { border-color: #2563eb; }
    input[type=file] { display: none; }
    .filename { font-weight: 600; color: #60a5fa; margin-top: 10px; word-break: break-all; }
    .btn { display: block; width: 100%; box-sizing: border-box; background: #2563eb; color: #fff; text-decoration: none; padding: 14px; border-radius: 8px; font-weight: bold; font-size: 16px; margin-top: 15px; border: none; cursor: pointer; }
    .btn:disabled { background: #4b5563; cursor: not-allowed; }
    .progress-box { display: none; margin-top: 20px; }
    .progress-bar-bg { background: #1f2937; border-radius: 8px; height: 20px; overflow: hidden; }
    .progress-bar { background: #10b981; width: 0%; height: 100%; transition: width 0.2s; }
    .status-text { font-size: 13px; color: #9ca3af; margin-top: 8px; text-align: center; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Firmware Update</h1>
    <p>Select a <code>firmware.bin</code> file to update your DashView unit wirelessly from your phone or computer.</p>
    
    <div class="dropzone" id="dz" onclick="document.getElementById('file-input').click()">
      <div id="prompt">&#128190; Tap to choose <b>firmware.bin</b></div>
      <div class="filename" id="fname"></div>
      <input type="file" id="file-input" accept=".bin" onchange="onFileSelected(this)">
    </div>

    <button id="upload-btn" class="btn" disabled onclick="startUpload()">Install Update</button>

    <div class="progress-box" id="pbox">
      <div class="progress-bar-bg"><div class="progress-bar" id="pbar"></div></div>
      <div class="status-text" id="pstatus">Uploading: 0%</div>
    </div>
  </div>

  <script>
    let selectedFile = null;
    function onFileSelected(input) {
      if (input.files && input.files[0]) {
        selectedFile = input.files[0];
        document.getElementById('fname').innerText = selectedFile.name + ' (' + Math.round(selectedFile.size / 1024) + ' KB)';
        document.getElementById('prompt').style.display = 'none';
        document.getElementById('upload-btn').disabled = false;
      }
    }

    function startUpload() {
      if (!selectedFile) return;
      document.getElementById('upload-btn').disabled = true;
      document.getElementById('pbox').style.display = 'block';

      let xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);

      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
          let pct = Math.round((e.loaded / e.total) * 100);
          document.getElementById('pbar').style.width = pct + '%';
          document.getElementById('pstatus').innerText = 'Writing Firmware: ' + pct + '% (Do not power off)';
        }
      };

      xhr.onload = function() {
        if (xhr.status == 200) {
          document.getElementById('pstatus').innerText = 'Update Successful! Rebooting DashView...';
          document.getElementById('pbar').style.background = '#10b981';
          setTimeout(function() { window.location.href = '/'; }, 8000);
        } else {
          document.getElementById('pstatus').innerText = 'Update Failed: ' + xhr.responseText;
          document.getElementById('pbar').style.background = '#ef4444';
          document.getElementById('upload-btn').disabled = false;
        }
      };

      xhr.onerror = function() {
        document.getElementById('pstatus').innerText = 'Network error during update.';
        document.getElementById('pbar').style.background = '#ef4444';
        document.getElementById('upload-btn').disabled = false;
      };

      let formData = new FormData();
      formData.append('update', selectedFile, selectedFile.name);
      xhr.send(formData);
    }
  </script>
</body>
</html>
)rawliteral";

static void handleRoot() {
    String html = FPSTR(INDEX_HTML);
    html.replace("APP_VER", APP_VERSION_STR);
    html.replace("VEHICLE_NAME", getProfileName());
    char sigBuf[16];
    snprintf(sigBuf, sizeof(sigBuf), "%d", getSignalCount());
    html.replace("SIG_COUNT", sigBuf);
    webServer.send(200, "text/html", html);
}

static void handleUpdatePage() {
    webServer.send(200, "text/html", FPSTR(UPDATE_HTML));
}

static void handleUpdateUpload() {
    HTTPUpload& upload = webServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        isUpdateInProgress = true;
        updateProgressPercent = 0;
        snprintf(updateStatusMessage, sizeof(updateStatusMessage), "Web OTA: %s", upload.filename.c_str());
        Serial.printf("[OTA] Starting Web OTA Update: %s\n", upload.filename.c_str());

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        if (upload.totalSize > 0) {
            updateProgressPercent = (upload.currentSize * 100) / upload.totalSize;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Success: %u bytes written. Rebooting...\n", upload.totalSize);
            snprintf(updateStatusMessage, sizeof(updateStatusMessage), "Update Complete! Rebooting...");
            updateProgressPercent = 100;
        } else {
            Update.printError(Serial);
            snprintf(updateStatusMessage, sizeof(updateStatusMessage), "OTA Error: End failed");
            isUpdateInProgress = false;
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        Serial.println("[OTA] Update was aborted.");
        snprintf(updateStatusMessage, sizeof(updateStatusMessage), "Update Aborted");
        isUpdateInProgress = false;
    }
}

static void handleUpdateResponse() {
    if (Update.hasError()) {
        webServer.send(500, "text/plain", "OTA Failed: " + String(Update.errorString()));
    } else {
        webServer.send(200, "text/plain", "SUCCESS");
        delay(500);
        ESP.restart();
    }
}

void initWebUpdater() {
    if (s_webServerStarted) return;

    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/update", HTTP_GET, handleUpdatePage);
    webServer.on("/update", HTTP_POST, handleUpdateResponse, handleUpdateUpload);

    webServer.begin();
    s_webServerStarted = true;
    Serial.println("[WEB] Web Management & OTA Portal running on http://192.168.4.1/ (and /update)");
}

void handleWebUpdater() {
    if (s_webServerStarted) {
        webServer.handleClient();
    }
}

bool checkSdUpdateAvailable(char* outFileName, size_t maxLen, size_t* outFileSize) {
    const char* candidates[] = {"/dashview.bin", "/update.bin", "/firmware.bin"};
    for (int i = 0; i < 3; i++) {
        if (SD.exists(candidates[i])) {
            File f = SD.open(candidates[i], FILE_READ);
            if (f) {
                size_t sz = f.size();
                f.close();
                // Valid ESP32-S3 app binaries are typically > 500 KB and < 6 MB
                if (sz > 500000 && sz < 6500000) {
                    if (outFileName) strncpy(outFileName, candidates[i], maxLen - 1);
                    if (outFileSize) *outFileSize = sz;
                    return true;
                }
            }
        }
    }
    return false;
}

bool performSdUpdate(const char* filename, void (*progressCb)(size_t written, size_t total)) {
    if (!filename || !SD.exists(filename)) return false;

    File f = SD.open(filename, FILE_READ);
    if (!f) return false;

    size_t total = f.size();
    if (total < 500000) { f.close(); return false; }

    Serial.printf("[SD-OTA] Flashing %s (%u bytes)...\n", filename, total);
    isUpdateInProgress = true;
    updateProgressPercent = 0;
    snprintf(updateStatusMessage, sizeof(updateStatusMessage), "SD Update: %s", filename);

    if (!Update.begin(total, U_FLASH)) {
        Update.printError(Serial);
        f.close();
        isUpdateInProgress = false;
        return false;
    }

    uint8_t buf[4096];
    size_t written = 0;
    while (f.available()) {
        size_t r = f.read(buf, sizeof(buf));
        if (r > 0) {
            Update.write(buf, r);
            written += r;
            updateProgressPercent = (written * 100) / total;
            if (progressCb) progressCb(written, total);
        }
    }
    f.close();

    if (Update.end(true)) {
        Serial.println("[SD-OTA] Firmware flashed successfully!");
        snprintf(updateStatusMessage, sizeof(updateStatusMessage), "Flash verified! Rebooting...");
        updateProgressPercent = 100;

        // Rename file to .bak so we don't flash in a loop
        char bakName[64];
        snprintf(bakName, sizeof(bakName), "%s.bak", filename);
        SD.remove(bakName);
        SD.rename(filename, bakName);

        delay(1000);
        ESP.restart();
        return true;
    } else {
        Update.printError(Serial);
        snprintf(updateStatusMessage, sizeof(updateStatusMessage), "SD Flash Failed");
        isUpdateInProgress = false;
        return false;
    }
}

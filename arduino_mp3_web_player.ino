// Bibliothèques nécessaires
#include <WiFi.h>
#include <WebServer.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioLibs/A2DPStream.h"
#include "AudioTools/Disk/AudioSourceSD.h"

// Configuration SPI et SD
const int SD_CS_PIN   = 13;
#define SPI_SCLK       14
#define SPI_MISO       2
#define SPI_MOSI       15

// Buffer audio et chemin des fichiers
const int BUFFER_SIZE = 15 * 1024;
const char* mp3Folder = "/mp3";
const char* fileExt   = "mp3";

// Serveur web HTTP
WebServer server(80);

// Outils audio
MP3DecoderHelix decoder;
BufferRTOS<uint8_t> buffer(BUFFER_SIZE);      // Allouer le buffer avec la taille définie
QueueStream<uint8_t> out(buffer);
AudioSourceSD source(mp3Folder, fileExt, true);
AudioPlayer player(source, out, decoder);
BluetoothA2DPSource a2dp_source;

// Point d'accès Wi‑Fi
const char* ssid     = "mp3-player";
const char* password = "12345678";

// Callback pour le streaming audio Bluetooth
int32_t get_data(uint8_t* data, int32_t bytes) {
  return buffer.readArray(data, bytes);
}

// Configuration du point d'accès Wi‑Fi
void setup_wifi_ap() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

// Fonction utilitaire pour servir un fichier via HTTP
void serve_file(const char* filepath, const char* mime) {
  File file = SD.open(filepath);
  if (!file) {
    server.send(404, "text/plain", "Fichier non trouvé");
    return;
  }
  server.streamFile(file, mime);
  file.close();
}

// Configuration des routes du serveur HTTP
void setup_web_server() {
  // Pages statiques
  server.on("/",            [](){ serve_file("/web/index.html",         "text/html"); });
  server.on("/style.css",   [](){ serve_file("/web/style.css",         "text/css"); });
  server.on("/app.js",      [](){ serve_file("/web/app.js",           "application/javascript"); });

  // Liste des fichiers MP3 sur la carte SD
  server.on("/list", []() {
    File root = SD.open(mp3Folder);
    if (!root || !root.isDirectory()) {
      server.send(500, "text/plain", "Erreur carte SD");
      return;
    }
    String json = "[";
    while (File file = root.openNextFile()) {
      if (!file.isDirectory()) {
        String name = file.name();
        if (name.endsWith(".mp3")) {
          if (json.length() > 1) json += ",";
          json += "\"" + name + "\"";
        }
      }
      file.close();
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  // Contrôles de lecture
  server.on("/play", []() {
    player.play();            // Reprendre la lecture sans réinitialiser
    server.send(200, "text/plain", "Lecture reprise");
  });
  server.on("/pause", []() {
    player.stop();             // Mettre en pause sans arrêter complètement
    server.send(200, "text/plain", "Pause");
  });
  server.on("/next", []() {
    player.next();
    server.send(200, "text/plain", "Titre suivant");
  });

  // Lecture d'un fichier spécifié
  server.on("/playfile", []() {
    if (!server.hasArg("name")) {
      server.send(400, "text/plain", "Param 'name' requis");
      return;
    }
    String name = server.arg("name");
    String path = String(mp3Folder) + "/" + name;
    if (!SD.exists(path)) {
      server.send(404, "text/plain", "Fichier introuvable");
      return;
    }
    player.stop();                    // Arrêter la lecture en cours
    source.selectStream(path.c_str());
    player.begin();                  // Commencer la lecture du nouveau fichier
    server.send(200, "text/plain", "Lecture " + name);
  });

  server.begin();
}

// Gestion du bouton play/pause via Bluetooth AVRCP
void playPauseToggle() {
  if (player.isActive()) {
    player.stop();
    Serial.println("→ Pause");
  } else {
    player.play();
    Serial.println("→ Lecture");
  }
}

void button_handler(uint8_t id, bool released) {
  if (released) {
    Serial.printf("button id %u released\n", id);
    switch (id) {
      case 75: player.previous(); break;  // AVRCP: Previous
      case 76: player.next();     break;  // AVRCP: Next
      case 68: playPauseToggle(); break;  // AVRCP: Play/Pause
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Initialisation SPI et SD
  SPI.begin(SPI_SCLK, SPI_MISO, SPI_MOSI, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("ERREUR: initialisation SD");
    while (1) delay(1000);
  }

  // Logger audio
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // Préparer le décodeur et le buffer
  decoder.begin();
  buffer.resize(BUFFER_SIZE);
  out.begin(95);

  // Initialiser le player
  player.setVolume(0.3);
  player.begin();                    // Lance automatiquement la lecture du premier fichier

  // Wi‑Fi et serveur web
  setup_wifi_ap();
  setup_web_server();

  // A2DP Bluetooth
  a2dp_source.set_data_callback(get_data);
  a2dp_source.set_avrc_passthru_command_callback(button_handler);
  a2dp_source.start("hama Freedom Lit");
}

void loop() {
  server.handleClient();
  player.copy();                     // Remplir le buffer audio
}

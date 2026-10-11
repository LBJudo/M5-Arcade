/*
  M5 Arcade v2 - M5Stick (ESP32-S3)
  A : clic = jeu suivant, appui long (1,5 s) = supprimer     B : change de mode
  Modes : AP JEU (portail captif) / SETUP (M5-Setup) / RESEAU LOCAL (+ admin sur /setup)
  Multijoueur : relais WebSocket sur /ws (jeux *2.html)  |  Plusieurs Wi-Fi  |  Carte SD
  Bibliotheques : M5Unified, ESP Async WebServer (ESP32Async), Async TCP (ESP32Async)
*/
#include <M5Unified.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>

// ---------- Configuration ----------
// Depot GitHub source des jeux : a saisir dans la page Setup (vide par defaut)
static const char* DEF_GH_USER = "";
static const char* DEF_GH_REPO = "";
static const char* DEF_SSID    = "";   // Wi-Fi par defaut (vide = a configurer dans la page Setup)
static const char* DEF_PASS    = "";
static const char* DEF_SETUP   = "1234";
// Carte SD (SPI) : renseigne tes 4 broches selon ton cablage. SD_CS = -1 => SD desactivee.
static const int SD_SCK = -1, SD_MISO = -1, SD_MOSI = -1, SD_CS = -1;
static const int MAX_NETS = 5;

enum Mode { MODE_GAME = 0, MODE_SETUP = 1, MODE_LAN = 2 };
struct Net { String s, p; };
struct Game { String name; bool sd; };

Preferences prefs;
AsyncWebServer* server = nullptr;
AsyncWebSocket ws("/ws");
DNSServer dns;

Mode mode = MODE_GAME;
std::vector<Game> games;
std::vector<Net> nets;
int cur = 0;
String curPath = "";
bool curSd = false, sdOk = false;
volatile bool dirtyGames = true, reloadMode = false;
uint32_t reloadAt = 0;
String setupPass, ghUser, ghRepo;
bool needBasicAuth = false;
String pendingName = "", dlStatus = "Pret";
bool screenDirty = true;
uint32_t lastDraw = 0, netT = 0;
int netIdx = 0;
uint32_t slotId[8] = {0};

// ---------- Utilitaires ----------
String esc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;"; else if (c == '&') o += "&amp;"; else o += c;
  }
  return o;
}
bool cleanName(String& n) {
  n.trim();
  if (n.length() == 0 || n.length() > 40) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  if (n.startsWith(".") || n.indexOf("..") >= 0) return false;
  if (!n.endsWith(".html")) n += ".html";
  return true;
}
bool okId(const String& s) {
  if (s.length() == 0 || s.length() > 60) return false;
  for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false; }
  return true;
}
String prm(AsyncWebServerRequest* r, const char* k, bool post) {
  return r->hasParam(k, post) ? r->getParam(k, post)->value() : String("");
}
FS& fsOf(bool sd) { return sd ? (FS&)SD : (FS&)LittleFS; }

// ---------- Config (plusieurs Wi-Fi) ----------
void saveNets() {
  prefs.putUChar("nn", nets.size());
  for (size_t i = 0; i < nets.size(); i++) {
    prefs.putString(("s" + String(i)).c_str(), nets[i].s);
    prefs.putString(("p" + String(i)).c_str(), nets[i].p);
  }
}
void loadConfig() {
  prefs.begin("arcade", false);
  setupPass = prefs.getString("spass", DEF_SETUP);
  ghUser = prefs.getString("ghu", DEF_GH_USER);
  ghRepo = prefs.getString("ghr", DEF_GH_REPO);
  uint8_t n = prefs.getUChar("nn", 255);
  if (n == 255) { if (strlen(DEF_SSID)) nets.push_back({DEF_SSID, DEF_PASS}); saveNets(); }
  else for (uint8_t i = 0; i < n && i < MAX_NETS; i++)
    nets.push_back({prefs.getString(("s" + String(i)).c_str(), ""), prefs.getString(("p" + String(i)).c_str(), "")});
}

// ---------- Jeux (LittleFS + SD) ----------
void scan(bool sd) {
  File dir = fsOf(sd).open("/games");
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    String n = String(f.name());
    int s = n.lastIndexOf('/');
    if (s >= 0) n = n.substring(s + 1);
    if (!f.isDirectory() && n.endsWith(".html") && !n.startsWith(".")) games.push_back({n, sd});
    f = dir.openNextFile();
  }
}
void selectGame() {
  if (games.empty()) { curPath = ""; return; }
  curPath = "/games/" + games[cur].name;
  curSd = games[cur].sd;
}
void refreshGames() {
  String keep = (cur >= 0 && cur < (int)games.size()) ? games[cur].name : "";
  games.clear();
  scan(false);
  if (sdOk) scan(true);
  std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return a.name < b.name; });
  cur = 0;
  for (size_t i = 0; i < games.size(); i++) if (games[i].name == keep) cur = i;
  selectGame();
  dirtyGames = false;
  screenDirty = true;
}
void serveGame(AsyncWebServerRequest* r) {
  if (curPath == "" || !fsOf(curSd).exists(curPath)) {
    r->send(200, "text/html",
      "<!DOCTYPE html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<h2>Aucun jeu installe</h2><p>Passe en mode Setup pour telecharger des jeux depuis GitHub.</p>");
    return;
  }
  r->send(fsOf(curSd), curPath, "text/html");
}

// ---------- Portail Setup ----------
bool auth(AsyncWebServerRequest* r) {
  if (mode == MODE_GAME) { r->send(404, "text/plain", "Setup indisponible en mode jeu"); return false; }
  if (needBasicAuth && !r->authenticate("admin", setupPass.c_str())) { r->requestAuthentication(); return false; }
  return true;
}
String setupPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>M5 Setup</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:auto;padding:12px}"
    "input,button{width:100%;padding:10px;margin:4px 0;box-sizing:border-box}"
    "fieldset{margin:12px 0}li{margin:6px 0}</style></head><body><h2>M5 Arcade - Setup</h2>"
    "<fieldset><legend>Reseaux Wi-Fi (max 5, essayes dans l'ordre)</legend><ul>");
  for (size_t i = 0; i < nets.size(); i++)
    h += "<li>" + esc(nets[i].s) + " - <a href=\"/wdel?i=" + String(i) + "\">supprimer</a></li>";
  h += F("</ul><form method=POST action=/wifi><input name=ssid placeholder=SSID>"
    "<input name=pass type=password placeholder='Mot de passe Wi-Fi'><button>Ajouter / mettre a jour</button></form></fieldset>"
    "<fieldset><legend>Mot de passe de Setup</legend><form method=POST action=/pwd>"
    "<input name=p placeholder='Nouveau mot de passe (4+ car.)'><button>Changer</button></form></fieldset>"
    "<fieldset><legend>Depot GitHub (source des jeux)</legend><form method=POST action=/gh>"
    "<input name=u placeholder='Utilisateur GitHub' value=\"");
  h += esc(ghUser);
  h += F("\"><input name=r placeholder='Depot (ex. games)' value=\"");
  h += esc(ghRepo);
  h += F("\"><button>Enregistrer le depot</button></form></fieldset>"
    "<fieldset><legend>Telecharger un jeu</legend><form method=POST action=/dl>"
    "<input name=name placeholder='pong.html'><button>Telecharger</button></form>"
    "<p id=st>");
  h += esc(dlStatus);
  h += String(F("</p><small>Stockage : ")) + (sdOk ? "carte SD" : "memoire interne") + F("</small></fieldset>"
    "<fieldset><legend>Jeux installes</legend><ul>");
  if (games.empty()) h += F("<li>Aucun jeu</li>");
  for (auto& g : games)
    h += "<li>" + esc(g.name) + (g.sd ? " [SD]" : "") + " - <a href=\"/del?f=" + g.name + "&s=" + (g.sd ? "1" : "0") +
         "\" onclick=\"return confirm('Supprimer ?')\">supprimer</a></li>";
  h += F("</ul></fieldset><script>setInterval(()=>fetch('/status').then(r=>r.text()).then(t=>{"
    "var e=document.getElementById('st');if(e.textContent!=t){if(/^OK/.test(t))location.reload();e.textContent=t;}}),2000)"
    "</script></body></html>");
  return h;
}

// ---------- Multijoueur : relais WebSocket ----------
int freeSlot() { for (int i = 0; i < 8; i++) if (!slotId[i]) return i; return -1; }
int slotOf(uint32_t id) { for (int i = 0; i < 8; i++) if (slotId[i] == id) return i; return -1; }
void sendMask() {
  int m = 0;
  for (int i = 0; i < 8; i++) if (slotId[i]) m |= 1 << i;
  ws.textAll("C" + String(m));
  screenDirty = true;
}
void wsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* c, AwsEventType t, void* arg, uint8_t* data, size_t len) {
  if (t == WS_EVT_CONNECT) {
    int s = freeSlot();
    if (s < 0) { c->close(); return; }
    slotId[s] = c->id();
    c->text("I" + String(s));
    sendMask();
  } else if (t == WS_EVT_DISCONNECT) {
    int s = slotOf(c->id());
    if (s >= 0) slotId[s] = 0;
    sendMask();
  } else if (t == WS_EVT_DATA) {
    AwsFrameInfo* i = (AwsFrameInfo*)arg;
    if (i->final && i->index == 0 && i->len == len && i->opcode == WS_TEXT) {
      String m; m.reserve(len);
      for (size_t k = 0; k < len; k++) m += (char)data[k];
      // Message cible "@<slot>|texte" : livre uniquement a ce joueur (ex. entrees -> hote)
      if (m.length() > 3 && m[0] == '@' && m[2] == '|') {
        int tg = m[1] - '0';
        if (tg >= 0 && tg < 8 && slotId[tg]) ws.text(slotId[tg], m.substring(3));
        return;
      }
      for (int k = 0; k < 8; k++) if (slotId[k] && slotId[k] != c->id()) ws.text(slotId[k], m);
    }
  }
}

// Le serveur est cree UNE SEULE FOIS (apres le demarrage du Wi-Fi) ; les handlers testent le mode.
void initServer() {
  server = new AsyncWebServer(80);
  ws.onEvent(wsEvent);
  server->addHandler(&ws);
  server->on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (mode != MODE_SETUP) { serveGame(r); return; }
    if (!auth(r)) return;
    r->send(200, "text/html", setupPage());
  });
  server->on("/setup", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    r->send(200, "text/html", setupPage());
  });
  server->on("/status", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", dlStatus); });
  server->on("/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String s = prm(r, "ssid", true), p = prm(r, "pass", true);
    s.trim();
    if (s.length()) {
      bool found = false;
      for (auto& n : nets) if (n.s == s) { n.p = p; found = true; }
      if (!found && (int)nets.size() < MAX_NETS) nets.push_back({s, p});
      saveNets();
    }
    r->redirect("/setup");
  });
  server->on("/wdel", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    int i = prm(r, "i", false).toInt();
    if (i >= 0 && i < (int)nets.size()) { nets.erase(nets.begin() + i); saveNets(); }
    r->redirect("/setup");
  });
  server->on("/pwd", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String p = prm(r, "p", true);
    if (p.length() >= 4 && p.length() <= 63) {
      setupPass = p; prefs.putString("spass", p);
      reloadMode = true; reloadAt = millis() + 1500;
      r->send(200, "text/html", "<meta charset=utf-8>Mot de passe change. Reconnecte-toi au Wi-Fi.");
      return;
    }
    r->redirect("/setup");
  });
  server->on("/dl", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String n = prm(r, "name", true);
    if (cleanName(n)) { pendingName = n; dlStatus = "En attente : " + n; } else dlStatus = "Nom invalide";
    r->redirect("/setup");
  });
  server->on("/gh", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String u = prm(r, "u", true), g = prm(r, "r", true);
    u.trim(); g.trim();
    if (okId(u) && okId(g)) { ghUser = u; ghRepo = g; prefs.putString("ghu", u); prefs.putString("ghr", g); dlStatus = "Depot enregistre"; }
    else dlStatus = "Depot invalide (lettres, chiffres, - _ . seulement)";
    r->redirect("/setup");
  });
  server->on("/del", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String n = prm(r, "f", false);
    if (cleanName(n)) { fsOf(prm(r, "s", false) == "1").remove("/games/" + n); dirtyGames = true; }
    r->redirect("/setup");
  });
  server->onNotFound([](AsyncWebServerRequest* r) {
    if (mode == MODE_SETUP) r->redirect("/"); else serveGame(r);
  });
  server->begin();
}

// ---------- Wi-Fi ----------
void lanBegin() {
  if (nets.empty()) return;
  netIdx %= nets.size();
  WiFi.begin(nets[netIdx].s.c_str(), nets[netIdx].p.c_str());
  netT = millis();
}
bool connectAny() {   // bloquant, utilise pour le telechargement
  if (WiFi.status() == WL_CONNECTED) return true;
  for (auto& n : nets) {
    dlStatus = "Connexion a " + n.s + "...";
    WiFi.begin(n.s.c_str(), n.p.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 9000) delay(250);
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect();
  }
  return false;
}

// ---------- Telechargement GitHub ----------
void doDownload(String name) {
  if (ghUser == "" || ghRepo == "") { dlStatus = "Configure d'abord le depot GitHub"; return; }
  if (!connectAny()) { dlStatus = "Echec : aucun Wi-Fi accessible"; return; }
  dlStatus = "Telechargement " + name + "...";
  bool toSd = sdOk, ok = false;
  FS& fs = fsOf(toSd);
  fs.mkdir("/games");
  const char* branches[] = {"main", "master"};
  for (auto b : branches) {
    String url = String("https://raw.githubusercontent.com/") + ghUser + "/" + ghRepo + "/" + b + "/" + name;
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) continue;
    if (http.GET() == 200) {
      String tmp = "/games/.tmp";
      File f = fs.open(tmp, "w");
      if (f) {
        int w = http.writeToStream(&f);
        f.close();
        if (w > 0) { String d = "/games/" + name; fs.remove(d); fs.rename(tmp, d); ok = true; }
        else fs.remove(tmp);
      }
    }
    http.end();
    if (ok) break;
  }
  dlStatus = ok ? "OK : " + name + (toSd ? " (SD)" : "") : "Echec : " + name + " introuvable";
  dirtyGames = true;
}

// ---------- Modes ----------
void stopAll() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(150);
}
void startMode(Mode m) {
  stopAll();
  mode = m;
  if (m == MODE_GAME) {
    needBasicAuth = false;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("M5-Arcade");
    delay(200);
    dns.start(53, "*", WiFi.softAPIP());
  } else if (m == MODE_SETUP) {
    WiFi.mode(WIFI_AP_STA);
    needBasicAuth = setupPass.length() < 8;   // WPA2 exige 8 car. minimum
    if (needBasicAuth) WiFi.softAP("M5-Setup"); else WiFi.softAP("M5-Setup", setupPass.c_str());
    delay(200);
    dns.start(53, "*", WiFi.softAPIP());
  } else {
    needBasicAuth = true;   // admin via http://IP/setup
    WiFi.mode(WIFI_STA);
    netIdx = 0;
    lanBegin();
  }
  screenDirty = true;
}

// ---------- Affichage ----------
void draw() {
  auto& d = M5.Display;
  d.fillScreen(BLACK);
  const char* names[] = {"AP JEU", "SETUP", "RESEAU LOCAL"};
  uint16_t cols[] = {GREEN, ORANGE, CYAN};
  d.setCursor(4, 4);
  d.setTextColor(cols[mode]); d.setTextSize(2); d.println(names[mode]);
  d.setTextSize(1); d.setTextColor(WHITE);
  if (mode == MODE_GAME) {
    d.println("Wi-Fi : M5-Arcade (ouvert)");
    d.println("IP : " + WiFi.softAPIP().toString());
  } else if (mode == MODE_SETUP) {
    d.println(needBasicAuth ? "Wi-Fi M5-Setup (admin/MdP)" : "Wi-Fi M5-Setup protege");
    d.println("MdP : " + setupPass);
    d.println("IP : " + WiFi.softAPIP().toString());
  } else if (WiFi.status() == WL_CONNECTED) {
    d.println("Reseau : " + WiFi.SSID());
    d.setTextColor(YELLOW); d.setTextSize(2); d.println(WiFi.localIP().toString());
    d.setTextSize(1); d.setTextColor(WHITE); d.println("Setup : IP/setup");
  } else {
    d.println(nets.empty() ? "Aucun Wi-Fi configure" : "Connexion : " + nets[netIdx % nets.size()].s + "...");
  }
  d.printf("Joueurs : %d  %s\n", (int)ws.count(), sdOk ? "SD OK" : (SD_CS >= 0 ? "SD absente" : ""));
  if (games.empty()) d.println("Aucun jeu en memoire");
  else {
    d.printf("Jeu %d/%d%s\n", cur + 1, (int)games.size(), games[cur].sd ? " [SD]" : "");
    d.setTextColor(YELLOW); d.println(games[cur].name);
  }
  d.setTextColor(DARKGREY);
  d.setCursor(4, d.height() - 10);
  d.print("A:suiv (long:suppr)  B:mode");
}

// ---------- Setup / loop ----------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.BtnA.setHoldThresh(1500);
  if (!LittleFS.begin(true)) M5.Display.println("Erreur LittleFS");
  LittleFS.mkdir("/games");
  if (SD_CS >= 0) {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdOk = SD.begin(SD_CS, SPI, 20000000);
    if (sdOk) SD.mkdir("/games");
  }
  loadConfig();
  refreshGames();
  startMode(MODE_GAME);   // le Wi-Fi doit etre demarre AVANT de creer le serveur
  initServer();
}

void loop() {
  M5.update();
  if (M5.BtnB.wasClicked()) startMode((Mode)((mode + 1) % 3));

  if (M5.BtnA.wasHold()) {
    if (!games.empty()) {
      Game g = games[cur];
      fsOf(g.sd).remove("/games/" + g.name);
      M5.Display.fillScreen(RED); M5.Display.setCursor(4, 40); M5.Display.setTextColor(WHITE);
      M5.Display.println("Supprime :"); M5.Display.println(g.name);
      delay(800);
      cur = 0; dirtyGames = true;
    }
  } else if (M5.BtnA.wasClicked() && !games.empty()) {
    cur = (cur + 1) % games.size();
    selectGame();
    screenDirty = true;
  }

  if (dirtyGames) refreshGames();
  if (pendingName != "") { String n = pendingName; pendingName = ""; doDownload(n); }
  if (reloadMode && millis() > reloadAt) { reloadMode = false; startMode(mode); }

  if (mode != MODE_LAN) dns.processNextRequest();
  else {
    if (WiFi.status() == WL_CONNECTED) netT = millis();
    else if (!nets.empty() && millis() - netT > 12000) { netIdx++; WiFi.disconnect(); lanBegin(); screenDirty = true; }
  }
  ws.cleanupClients();

  if (screenDirty || millis() - lastDraw > 1000) { draw(); lastDraw = millis(); screenDirty = false; }
  delay(5);
}

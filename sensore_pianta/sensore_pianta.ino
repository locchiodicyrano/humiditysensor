/*
  Sensore di umidità per piante – ESP32-C3 SuperMini
  ---------------------------------------------------
  - Misura l'umidità della terra e avvisa su Telegram.
  - Ogni mattina manda lo stato con il grafico degli ultimi 7 giorni.
  - Si aggiorna da solo scaricando il firmware dalle Release di GitHub.
  - Per risparmiare batteria il Wi-Fi si accende solo quando c'è da mandare
    un messaggio: niente comandi Telegram (la scheda non ascolta).
  - Wi-Fi, token, chat e nome della pianta NON sono nel codice: si scrivono
    una volta sola dal monitor seriale e restano salvati nella scheda (NVS),
    anche dopo gli aggiornamenti.

  Impostazioni Arduino IDE: scheda "ESP32C3 Dev Module", USB CDC On Boot: Enabled,
  Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)", Flash Mode: DIO.
  Libreria da installare: ArduinoJson (di Benoit Blanchon), versione 7.
*/

// Creato in automatico da GitHub Actions con numero di versione e repository.
// Quando compili a mano con Arduino IDE non esiste e valgono i valori qui sotto.
#if __has_include("firmware_info.h")
#include "firmware_info.h"
#endif

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"

// ===== VERSIONE E REPOSITORY =====
#ifndef VERSIONE_FIRMWARE
#define VERSIONE_FIRMWARE 0        // 0 = compilato a mano: il mattino dopo viene sostituito dalla versione di GitHub
#endif
#ifndef REPO_GITHUB
#define REPO_GITHUB "locchiodicyrano/humiditysensor"   // il tuo "utente/repository" su GitHub
#endif

// true  = PROVE: niente sonno, il monitor seriale resta collegato
//         e mostra la lettura ogni 2 secondi
// false = USO NORMALE a batteria con sonno profondo
// (le versioni compilate da GitHub sono sempre in uso normale)
#ifndef DEBUG
#define DEBUG false
#endif

// ===== IMPOSTAZIONI UGUALI PER TUTTE LE PIANTE =====
// (quelle diverse per ogni pianta si scrivono dal monitor seriale)

// Valori iniziali per taratura e soglie: si possono cambiare dal monitor seriale
const int VALORE_SECCO_INIZIALE   = 3400;  // lettura in aria (sensore alimentato dal pin 3)
const int VALORE_BAGNATO_INIZIALE = 1790;  // lettura in acqua
const int SOGLIA_SECCO_INIZIALE   = 10;    // sotto questa % -> "annaffiami"
const int SOGLIA_BAGNATO_INIZIALE = 35;    // sopra questa % -> "grazie"

// true solo dopo aver montato il partitore (2 resistenze da 100k sul pin 1)
#define USA_BATTERIA true
const float SOGLIA_BATTERIA = 3.6;   // sotto questi volt -> "batteria scarica"
const float BATTERIA_CARICA = 3.9;   // sopra questi volt l'avviso si azzera (dopo la ricarica)
const float BATTERIA_MIN_AGGIORNAMENTO = 3.7;  // sotto questi volt non si aggiorna il firmware

const int MINUTI_SONNO     = 60;    // ogni quanto controlla: 2 per le prove, 60 per l'uso normale
const int MINUTI_CAMPIONE  = 60;    // ogni quanto salva una misura per i grafici (7 giorni = 168 misure)

// Messaggio di stato giornaliero con il grafico: parte al primo controllo dopo
// quest'ora (con MINUTI_SONNO = 60 arriva tra le 8:00 e le 9:00).
// Nello stesso momento controlla se su GitHub c'è un firmware nuovo.
const int ORA_STATUS       = 8;

// Fuso orario italiano, con passaggio automatico all'ora legale
const char* FUSO_ORARIO    = "CET-1CEST,M3.5.0,M10.5.0/3";
// =============================

#define PIN_SENSORE 0   // ingresso analogico (ADC1, nessun conflitto col Wi-Fi sul C3)
#define PIN_ALIM    3   // alimenta il sensore solo durante la lettura (corrente massima)
#define PIN_BATT    1   // centro del partitore 100k + 100k sulla batteria

// ---------- Configurazione salvata nella scheda ----------

struct Config {
  String ssid, pass, token, chat, nome;
  int secco, bagnato, sogliaSecco, sogliaBagnato;
};
Config cfg;

// ---------- Stato e storico (salvati in flash, restano anche senza batteria) ----------

const int NUM_CAMPIONI = 168;          // 7 giorni con una misura all'ora

struct Campione {
  uint32_t t;        // ora della misura (secondi dal 1970)
  uint16_t mv;       // tensione della batteria in millivolt
  uint16_t grezzo;   // lettura del sensore
};

struct Stato {
  uint32_t magic;
  bool     giaAvvisato;
  bool     batteriaAvvisata;
  int32_t  giornoUltimoStatus;   // giorno dell'ultimo messaggio di stato
  int32_t  risvegliSenzaOra;     // per riprovare a leggere l'ora se manca
  uint16_t indice;              // dove va la prossima misura
  uint16_t quanti;               // quante misure ci sono
  Campione storico[NUM_CAMPIONI];
};

const uint32_t MAGIC_STATO = 0x50540000u + sizeof(Stato);  // cambia se cambia la struttura
Stato st;
Stato stSalvato;     // copia dell'ultimo salvataggio: si riscrive la flash solo se serve

// Dichiarazione della funzione che rimanda la conferma del firmware nuovo
// (vedi "Protezione aggiornamenti" in fondo)
extern "C" bool verifyRollbackLater();

const char* GIORNI[] = {"dom", "lun", "mar", "mer", "gio", "ven", "sab"};

// ---------- Configurazione ----------

void caricaConfig() {
  Preferences p;
  p.begin("config", true);
  cfg.ssid          = p.getString("ssid", "");
  cfg.pass          = p.getString("pass", "");
  cfg.token         = p.getString("token", "");
  cfg.chat          = p.getString("chat", "");
  cfg.nome          = p.getString("nome", "");
  cfg.secco         = p.getInt("secco", VALORE_SECCO_INIZIALE);
  cfg.bagnato       = p.getInt("bagnato", VALORE_BAGNATO_INIZIALE);
  cfg.sogliaSecco   = p.getInt("s_secco", SOGLIA_SECCO_INIZIALE);
  cfg.sogliaBagnato = p.getInt("s_bagnato", SOGLIA_BAGNATO_INIZIALE);
  p.end();
}

bool configCompleta() {
  return cfg.ssid.length() && cfg.token.length() && cfg.chat.length() && cfg.nome.length();
}

// ---------- Stato ----------

void caricaStato() {
  Preferences p;
  bool ok = false;
  if (p.begin("stato", true)) {
    ok = p.getBytesLength("dati") == sizeof(Stato) &&
         p.getBytes("dati", &st, sizeof(Stato)) == sizeof(Stato) &&
         st.magic == MAGIC_STATO;
    p.end();
  }
  if (!ok) {
    memset(&st, 0, sizeof(Stato));
    st.magic = MAGIC_STATO;
    st.giornoUltimoStatus = -1;
  }
  memcpy(&stSalvato, &st, sizeof(Stato));
}

void salvaStato() {
  if (memcmp(&st, &stSalvato, sizeof(Stato)) == 0) return;   // niente di nuovo
  Preferences p;
  if (p.begin("stato", false)) {
    p.putBytes("dati", &st, sizeof(Stato));
    p.end();
    memcpy(&stSalvato, &st, sizeof(Stato));
  }
}

// Misura numero i, dalla più vecchia (0) alla più recente (quanti-1)
Campione& campione(int i) {
  return st.storico[(st.indice + NUM_CAMPIONI - st.quanti + i) % NUM_CAMPIONI];
}

// ---------- Sensore e batteria ----------

int leggiGrezzo() {
  pinMode(PIN_ALIM, OUTPUT);
  // corrente massima sul pin: senza questa riga la tensione scende a 2,9 V
  // e il sensore (regolatore bypassato) non funziona
  gpio_set_drive_capability((gpio_num_t)PIN_ALIM, GPIO_DRIVE_CAP_3);
  digitalWrite(PIN_ALIM, HIGH);          // accende il sensore
  delay(500);                            // attesa di sicurezza
  long somma = 0;
  for (int i = 0; i < 20; i++) { somma += analogRead(PIN_SENSORE); delay(10); }
  digitalWrite(PIN_ALIM, LOW);           // spegne il sensore
  return somma / 20;
}

// Tensione della batteria in volt (il partitore la dimezza, quindi x2)
float leggiBatteria() {
  long somma = 0;
  for (int i = 0; i < 20; i++) { somma += analogReadMilliVolts(PIN_BATT); delay(5); }
  return (somma / 20) * 2 / 1000.0;
}

// Percentuale approssimativa di una 18650 in base alla tensione
int percentualeBatteria(float v) {
  const float tv[] = {3.30, 3.50, 3.60, 3.65, 3.70, 3.75, 3.80, 3.90, 4.00, 4.10, 4.20};
  const int   tp[] = {   0,    5,   10,   20,   30,   40,   50,   65,   80,   90,  100};
  if (v <= tv[0])  return 0;
  if (v >= tv[10]) return 100;
  for (int i = 1; i < 11; i++) {
    if (v < tv[i]) {
      return tp[i-1] + (v - tv[i-1]) * (tp[i] - tp[i-1]) / (tv[i] - tv[i-1]);
    }
  }
  return 100;
}

// Riga con la batteria da aggiungere ai messaggi (vuota se il partitore non c'e')
String rigaBatteria() {
  if (!USA_BATTERIA) return "";
  float v = leggiBatteria();
  int p = percentualeBatteria(v);
  String icona = (p <= 15) ? "🪫" : "🔋";
  return String("\n") + icona + " Batteria: <b>" + p + "%</b> (" + String(v, 2) + " V)";
}

int percentuale(int grezzo) {
  int perc = map(grezzo, cfg.secco, cfg.bagnato, 0, 100);
  return constrain(perc, 0, 100);
}

// Nome della pianta pronto per i messaggi (i simboli < > & romperebbero il grassetto)
String nomeHtml() {
  String s = cfg.nome;
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  return s;
}

// ---------- Wi-Fi e ora ----------

// Si collega solo se non e' gia' collegato (piu' messaggi nello stesso
// risveglio usano la stessa connessione)
bool collegaWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("Mi collego al WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  int tentativi = 0;
  while (WiFi.status() != WL_CONNECTED && tentativi++ < 40) delay(250);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi NON collegato");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  Serial.printf("WiFi OK, segnale %d dBm\n", WiFi.RSSI());
  return true;
}

void spegniWiFi() {
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

// Il fuso orario va reimpostato a ogni risveglio (si perde nel sonno profondo,
// mentre l'orologio continua a contare)
void impostaFuso() {
  setenv("TZ", FUSO_ORARIO, 1);
  tzset();
}

// Legge l'ora da internet. Restituisce true se ci e' riuscito.
bool sincronizzaOra() {
  if (!collegaWiFi()) return false;
  Serial.println("Leggo l'ora da internet...");
  configTzTime(FUSO_ORARIO, "pool.ntp.org", "time.google.com");
  unsigned long t0 = millis();
  // Attenzione: "completato" si può leggere una volta sola, poi lo stato si azzera
  bool ok = false;
  while (!ok && millis() - t0 < 10000) {
    ok = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
    if (!ok) delay(100);
  }
  Serial.println(ok ? "Ora aggiornata" : "Ora NON aggiornata");
  return ok;
}

// Ora locale attuale. Restituisce false se l'ora non e' ancora nota.
bool oraLocale(struct tm &t) {
  time_t adesso;
  time(&adesso);
  localtime_r(&adesso, &t);
  return t.tm_year > (2024 - 1900);
}

// Numero che identifica il giorno (anno + giorno dell'anno)
int codiceGiorno(const struct tm &t) {
  return t.tm_year * 1000 + t.tm_yday;
}

// ---------- Storico per i grafici ----------

// Salva una misura, al massimo una ogni MINUTI_CAMPIONE minuti
void aggiungiCampione(int grezzo, float volt) {
  struct tm t;
  if (!oraLocale(t)) return;                 // senza l'ora la misura non serve al grafico
  uint32_t adesso = (uint32_t)time(nullptr);
  if (st.quanti > 0) {
    uint32_t ultimo = campione(st.quanti - 1).t;
    if (adesso >= ultimo && adesso - ultimo < (uint32_t)MINUTI_CAMPIONE * 60 * 9 / 10) return;
  }
  Campione& c = st.storico[st.indice];
  c.t = adesso;
  c.mv = (uint16_t)(volt * 1000);
  c.grezzo = (uint16_t)grezzo;
  st.indice = (st.indice + 1) % NUM_CAMPIONI;
  if (st.quanti < NUM_CAMPIONI) st.quanti++;
}

// ---------- Telegram ----------

// Codifica il testo per l'indirizzo web (serve per emoji, a capo, accenti...)
String urlEncode(const String& testo) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < testo.length(); i++) {
    uint8_t c = testo[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Chiama un metodo delle API di Telegram. Restituisce il codice HTTP
// (200 = tutto bene) e, se serve, il testo della risposta.
int chiamaTelegram(const String& metodo, const String& parametri, String* risposta) {
  if (!collegaWiFi()) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  String url = "https://api.telegram.org/bot" + cfg.token + "/" + metodo + "?" + parametri;
  if (!http.begin(client, url)) return -1;
  int codice = http.GET();
  if (risposta != nullptr && codice > 0) *risposta = http.getString();
  http.end();
  return codice;
}

// Restituisce true solo se Telegram ha ricevuto il messaggio.
// Il Wi-Fi resta acceso: si spegne prima di andare a dormire.
bool inviaTelegram(const String& testo) {
  // parse_mode=HTML permette il grassetto con <b>...</b>
  int codice = chiamaTelegram("sendMessage",
                              "chat_id=" + cfg.chat + "&parse_mode=HTML&text=" + urlEncode(testo),
                              nullptr);
  Serial.printf("Risposta Telegram: %d\n", codice);
  return codice == 200;
}

// Manda un'immagine presa da un indirizzo web, con una didascalia
bool inviaFoto(const String& urlFoto, const String& didascalia) {
  int codice = chiamaTelegram("sendPhoto",
                              "chat_id=" + cfg.chat + "&parse_mode=HTML&photo=" + urlEncode(urlFoto) +
                              "&caption=" + urlEncode(didascalia),
                              nullptr);
  Serial.printf("Risposta Telegram (foto): %d\n", codice);
  return codice == 200;
}

// ---------- Grafico e statistiche ----------

// Prepara il grafico su quickchart.io e restituisce l'indirizzo dell'immagine
// (vuoto se qualcosa va storto). L'immagine resta disponibile 3 giorni.
String creaGrafico(const String& titolo) {
  JsonDocument d;
  d["version"] = "2";
  d["backgroundColor"] = "white";
  d["width"] = 800;
  d["height"] = 420;
  d["format"] = "png";

  JsonObject chart = d["chart"].to<JsonObject>();
  chart["type"] = "line";
  JsonObject data = chart["data"].to<JsonObject>();
  JsonArray etichette = data["labels"].to<JsonArray>();
  JsonArray datasets = data["datasets"].to<JsonArray>();

  JsonObject dsUmid = datasets.add<JsonObject>();
  dsUmid["label"] = "Umidità %";
  dsUmid["borderColor"] = "#2b7bb9";
  dsUmid["backgroundColor"] = "rgba(43,123,185,0.12)";
  dsUmid["fill"] = true;
  dsUmid["pointRadius"] = 0;
  dsUmid["borderWidth"] = 2;
  JsonArray valUmid = dsUmid["data"].to<JsonArray>();

  JsonArray valBatt;
  if (USA_BATTERIA) {
    JsonObject dsBatt = datasets.add<JsonObject>();
    dsBatt["label"] = "Batteria %";
    dsBatt["borderColor"] = "#3a9d5d";
    dsBatt["fill"] = false;
    dsBatt["pointRadius"] = 0;
    dsBatt["borderWidth"] = 2;
    valBatt = dsBatt["data"].to<JsonArray>();
  }

  JsonObject dsSoglia = datasets.add<JsonObject>();
  dsSoglia["label"] = "Soglia secco";
  dsSoglia["borderColor"] = "#d9534f";
  dsSoglia["fill"] = false;
  dsSoglia["pointRadius"] = 0;
  dsSoglia["borderWidth"] = 1;
  JsonArray tratteggio = dsSoglia["borderDash"].to<JsonArray>();
  tratteggio.add(6);
  tratteggio.add(4);
  JsonArray valSoglia = dsSoglia["data"].to<JsonArray>();

  // Etichetta solo sulla prima misura di ogni giorno ("lun 10")
  int giornoPrec = -1;
  for (int i = 0; i < st.quanti; i++) {
    Campione& c = campione(i);
    time_t tt = c.t;
    struct tm t;
    localtime_r(&tt, &t);
    if (t.tm_yday != giornoPrec) {
      String etichetta = String(GIORNI[t.tm_wday]) + " " + t.tm_mday;
      etichette.add(etichetta);
      giornoPrec = t.tm_yday;
    } else {
      etichette.add("");
    }
    valUmid.add(percentuale(c.grezzo));
    if (USA_BATTERIA) valBatt.add(percentualeBatteria(c.mv / 1000.0));
    valSoglia.add(cfg.sogliaSecco);
  }

  JsonObject opzioni = chart["options"].to<JsonObject>();
  opzioni["title"]["display"] = true;
  opzioni["title"]["text"] = titolo;
  opzioni["legend"]["position"] = "bottom";
  JsonObject asseY = opzioni["scales"]["yAxes"].add<JsonObject>();
  asseY["ticks"]["min"] = 0;
  asseY["ticks"]["max"] = 100;
  asseY["ticks"]["stepSize"] = 20;
  JsonObject asseX = opzioni["scales"]["xAxes"].add<JsonObject>();
  asseX["ticks"]["autoSkip"] = false;
  asseX["ticks"]["maxRotation"] = 0;
  asseX["gridLines"]["display"] = false;

  String corpo;
  serializeJson(d, corpo);
  d.clear();

  if (!collegaWiFi()) return "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, "https://quickchart.io/chart/create")) return "";
  http.addHeader("Content-Type", "application/json");
  int codice = http.POST(corpo);
  String risposta = (codice == 200) ? http.getString() : "";
  http.end();
  Serial.printf("Risposta QuickChart: %d\n", codice);

  JsonDocument r;
  if (risposta.length() == 0 || deserializeJson(r, risposta)) return "";
  if (!(r["success"] | false)) return "";
  String url = r["url"] | "";
  return url;
}

// Periodo coperto dalle misure salvate, per esempio "ultimi 7 giorni"
String periodoStorico() {
  if (st.quanti < 2) return "";
  float ore = (campione(st.quanti - 1).t - campione(0).t) / 3600.0;
  return (ore < 48) ? String("ultime ") + (int)(ore + 0.5) + " ore"
                    : String("ultimi ") + (int)(ore / 24 + 0.5) + " giorni";
}

// Righe con il riepilogo degli ultimi 7 giorni (vuoto se ci sono poche misure)
String riepilogoSettimana() {
  int n = st.quanti;
  if (n < 2) return "";

  int uMin = 100, uMax = 0, annaffiature = 0, uPrec = -1;
  long uSomma = 0;
  for (int i = 0; i < n; i++) {
    int u = percentuale(campione(i).grezzo);
    uMin = min(uMin, u);
    uMax = max(uMax, u);
    uSomma += u;
    if (uPrec >= 0 && u - uPrec >= 15) annaffiature++;   // salto in su = acqua
    uPrec = u;
  }

  String periodo = periodoStorico();
  periodo.setCharAt(0, 'U');                              // "ultimi" -> "Ultimi"
  String testo = String("\n\n📊 <b>") + periodo + "</b>\n" +
                 "💧 Umidità: min " + uMin + "%, max " + uMax + "%, media " + (int)(uSomma / n) + "%\n" +
                 "🚿 Annaffiature rilevate: " + annaffiature;

  if (USA_BATTERIA) {
    // Un salto in su della tensione vuol dire ricarica o batteria nuova:
    // il consumo si calcola solo sulle misure successive all'ultimo salto
    const int SALTO_RICARICA_MV = 150;
    int primo = 0;
    for (int i = 1; i < n; i++) {
      if ((int)campione(i).mv - (int)campione(i - 1).mv >= SALTO_RICARICA_MV) primo = i;
    }
    int m = n - primo;

    // Media delle prime e delle ultime misure, per togliere un po' di rumore.
    // Anche il tempo trascorso è la media delle stesse misure.
    int k = max(1, min(6, m / 2));
    long mvInizio = 0, mvFine = 0;
    uint32_t secondi = 0;
    for (int i = 0; i < k; i++) {
      mvInizio += campione(primo + i).mv;
      mvFine   += campione(n - 1 - i).mv;
      secondi  += campione(n - 1 - i).t - campione(primo + i).t;
    }
    float vInizio = mvInizio / (float)k / 1000.0;
    float vFine   = mvFine   / (float)k / 1000.0;
    int pInizio = percentualeBatteria(vInizio);
    int pFine   = percentualeBatteria(vFine);
    float giorni = secondi / (float)k / 86400.0;

    if (giorni >= 1.0) {
      float consumo = (pInizio - pFine) / giorni;                 // % al giorno
      float mvGiorno = (vInizio - vFine) * 1000.0 / giorni;       // mV al giorno
      if (consumo > 0.05) {
        testo += String("\n📉 Consumo: circa ") + String(consumo, 1) + "% al giorno (" +
                 (int)(mvGiorno + 0.5) + " mV/giorno)";
        testo += String("\n⏳ Autonomia stimata: circa ") + (int)(pFine / consumo) + " giorni";
      } else {
        testo += "\n📉 Consumo: troppo basso per stimarlo, serve più tempo";
      }
    } else {
      testo += "\n📉 Consumo: lo stimo dopo almeno un giorno di misure";
    }
  }
  return testo;
}

// ---------- Aggiornamento del firmware da GitHub ----------

// Numero di versione in forma leggibile: 10203 -> "1.2.3"
String testoVersione(int v) {
  if (v == 0) return "0";
  return String(v / 10000) + "." + (v / 100 % 100) + "." + (v % 100);
}

String urlRelease(const char* file) {
  return String("https://github.com/") + REPO_GITHUB + "/releases/latest/download/" + file;
}

// Numero della versione pubblicata su GitHub (-1 se non si riesce a leggerlo)
int versioneDisponibile() {
  if (!collegaWiFi()) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub rimanda a un altro indirizzo
  if (!http.begin(client, urlRelease("version.txt"))) return -1;
  int codice = http.GET();
  int v = -1;
  if (codice == 200) {
    String s = http.getString();
    s.trim();
    v = s.toInt();
    if (v <= 0) v = -1;
  }
  http.end();
  Serial.printf("Versione su GitHub: %d (codice %d)\n", v, codice);
  return v;
}

// Una volta al giorno, dopo il messaggio del mattino (il Wi-Fi è già acceso).
// Scrive su Telegram solo se installa qualcosa o se l'installazione fallisce.
void controllaAggiornamento() {
  if (DEBUG) return;                                       // durante le prove non si aggiorna
  if (String(REPO_GITHUB) == "UTENTE/REPOSITORY") return;  // repository non impostato

  int nuova = versioneDisponibile();
  if (nuova <= VERSIONE_FIRMWARE) return;                  // già aggiornato (o GitHub non risponde)
  if (USA_BATTERIA && leggiBatteria() < BATTERIA_MIN_AGGIORNAMENTO) {
    Serial.println("Batteria troppo bassa per aggiornare");
    return;
  }

  inviaTelegram(String("🔄 <b>") + nomeHtml() + "</b>: installo il firmware v" + testoVersione(nuova) +
                " (ora ho la v" + testoVersione(VERSIONE_FIRMWARE) + "). Torno online tra un minuto.");
  salvaStato();

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return esito = httpUpdate.update(client, urlRelease("firmware.bin"));

  if (esito == HTTP_UPDATE_OK) {
    Serial.println("Aggiornamento installato, riavvio");
    spegniWiFi();
    delay(200);
    ESP.restart();
  }
  String errore = httpUpdate.getLastErrorString();
  Serial.println("Aggiornamento fallito: " + errore);
  inviaTelegram(String("⚠️ <b>") + nomeHtml() + "</b>: aggiornamento non riuscito (" + errore + ").");
}

// ---------- Controlli ----------

// Avviso batteria scarica (una sola volta, si azzera dopo la ricarica)
void controllaBatteria(float v) {
  if (!USA_BATTERIA) return;
  Serial.printf("Batteria: %.2f V\n", v);

  if (v < SOGLIA_BATTERIA && !st.batteriaAvvisata) {
    String msg = String("🪫 <b>") + nomeHtml() + "</b>: batteria quasi scarica!\n" +
                 "🔋 Carica: <b>" + percentualeBatteria(v) + "%</b> (" + String(v, 2) + " V)\n" +
                 "🔌 Ricaricami presto, se no non posso più avvisarti!";
    st.batteriaAvvisata = inviaTelegram(msg);
  }
  else if (v > BATTERIA_CARICA) {
    st.batteriaAvvisata = false;
  }
}

// Messaggio del mattino: stato di adesso, riepilogo della settimana e grafico,
// tutto in un solo messaggio. Restituisce true se Telegram l'ha ricevuto.
bool inviaMessaggioMattino(int grezzo, int umidita) {
  String testo = String("☀️ <b>") + nomeHtml() + "</b>: buongiorno!\n" +
                 "💧 Umidità del terreno: <b>" + umidita + "%</b> (lettura " + grezzo + ")" +
                 rigaBatteria() + "\n" +
                 (st.giaAvvisato ? "🥀 Sto ancora aspettando l'acqua!" : "🌿 Tutto bene, a domani!") +
                 riepilogoSettimana() +
                 "\n⚙️ Firmware v" + testoVersione(VERSIONE_FIRMWARE);

  if (st.quanti >= 2) {
    String url = creaGrafico(cfg.nome + " - " + periodoStorico());
    if (url.length() > 0 && inviaFoto(url, testo)) return true;
  }
  return inviaTelegram(testo);    // senza grafico (poche misure o quickchart non risponde)
}

// Messaggio del mattino una volta al giorno, al primo controllo dopo ORA_STATUS.
// Subito dopo controlla se su GitHub c'è un firmware nuovo.
void controllaStatusGiornaliero(int grezzo, int umidita) {
  struct tm t;
  if (!oraLocale(t)) {
    // Ora sconosciuta (internet non raggiungibile all'accensione):
    // riprova ogni 6 risvegli, per non consumare batteria ogni ora
    if (++st.risvegliSenzaOra < 6) return;
    st.risvegliSenzaOra = 0;
    if (!sincronizzaOra() || !oraLocale(t)) return;
  }

  int oggi = codiceGiorno(t);
  Serial.printf("Ora: %02d:%02d\n", t.tm_hour, t.tm_min);
  if (t.tm_hour < ORA_STATUS || oggi == st.giornoUltimoStatus) return;

  if (inviaMessaggioMattino(grezzo, umidita)) {
    st.giornoUltimoStatus = oggi;   // se fallisce, riprova al prossimo controllo
    salvaStato();
    sincronizzaOra();               // il Wi-Fi e' gia' acceso: corregge l'orologio
    controllaAggiornamento();
  }
}

// Il controllo vero e proprio: decide se mandare un messaggio
void controlla() {
  float volt  = USA_BATTERIA ? leggiBatteria() : 0;
  controllaBatteria(volt);

  int grezzo  = leggiGrezzo();
  int umidita = percentuale(grezzo);
  Serial.printf("CONTROLLO -> grezzo %d, umidita %d%%, gia avvisato: %s\n",
                grezzo, umidita, st.giaAvvisato ? "si" : "no");
  aggiungiCampione(grezzo, volt);

  if (umidita < cfg.sogliaSecco && !st.giaAvvisato) {
    // Terra secca: chiede da bere (una sola volta)
    String msg = String("🥀 <b>") + nomeHtml() + "</b> ha sete!\n" +
                 "💧 Umidità del terreno: <b>" + umidita + "%</b>" +
                 rigaBatteria() + "\n" +
                 "🚿 Annaffiami, per favore! 🙏";
    st.giaAvvisato = inviaTelegram(msg);
  }
  else if (umidita > cfg.sogliaBagnato && st.giaAvvisato) {
    // Annaffiata dopo l'avviso: ringrazia
    String msg = String("🌿 <b>") + nomeHtml() + "</b>: grazie per l'acqua! 😊\n" +
                 "💧 Umidità del terreno: <b>" + umidita + "%</b>" +
                 rigaBatteria() + "\n" +
                 "🌞 Ora sto benissimo!";
    if (inviaTelegram(msg)) st.giaAvvisato = false;   // se fallisce, riprova al prossimo controllo
  }

  controllaStatusGiornaliero(grezzo, umidita);
  salvaStato();
}

// ---------- Configurazione dal monitor seriale ----------

// Legge una riga dal monitor seriale (vuota se scade il tempo)
String leggiRiga(unsigned long attesaMs) {
  String riga;
  unsigned long t0 = millis();
  while (millis() - t0 < attesaMs) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (riga.length()) return riga;
      } else {
        riga += c;
      }
    }
    delay(10);
  }
  return riga;
}

void mostraConfig() {
  Serial.println("\n--- Impostazioni salvate ---");
  Serial.printf ("ssid           = %s\n", cfg.ssid.c_str());
  if (cfg.pass.length()) Serial.printf("password       = ******** (%u caratteri)\n", cfg.pass.length());
  else                   Serial.println("password       = (vuota)");
  if (cfg.token.length() > 8) Serial.printf("token          = %s...\n", cfg.token.substring(0, 6).c_str());
  else                        Serial.printf("token          = %s\n", cfg.token.c_str());
  Serial.printf ("chat           = %s\n", cfg.chat.c_str());
  Serial.printf ("nome           = %s\n", cfg.nome.c_str());
  Serial.printf ("secco          = %d   (lettura in aria)\n", cfg.secco);
  Serial.printf ("bagnato        = %d   (lettura in acqua)\n", cfg.bagnato);
  Serial.printf ("soglia_secco   = %d %%\n", cfg.sogliaSecco);
  Serial.printf ("soglia_bagnato = %d %%\n", cfg.sogliaBagnato);
  Serial.printf ("Firmware v%s, repository %s\n", testoVersione(VERSIONE_FIRMWARE).c_str(), REPO_GITHUB);
  if (!configCompleta()) Serial.println(">> Mancano ancora: ssid, token, chat e nome sono obbligatori");
}

void aiutoConfig() {
  Serial.println("\nScrivi una riga alla volta e premi Invio (fine riga: 'Nuova riga'):");
  Serial.println("  ssid=NomeRete");
  Serial.println("  password=PasswordWiFi");
  Serial.println("  token=TokenDelBot");
  Serial.println("  chat=IdDellaChat");
  Serial.println("  nome=Nome della pianta");
  Serial.println("  secco=3400  bagnato=1790  soglia_secco=10  soglia_bagnato=35");
  Serial.println("Altri comandi: mostra, lettura (legge il sensore), mattino (manda subito il");
  Serial.println("messaggio del mattino con il grafico), cancella (cancella tutto), fine");
}

void configurazioneSeriale() {
  // L'USB è collegata a un PC? (a batteria si salta dopo 2 secondi)
  unsigned long t0 = millis();
  while (!Serial.isPlugged() && millis() - t0 < 2000) delay(10);
  if (!Serial.isPlugged()) return;

  // Dopo RST il monitor seriale impiega qualche secondo a riagganciarsi
  t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  if (!Serial) return;

  if (configCompleta()) {
    // Il messaggio si ripete, così si vede anche se il monitor arriva in ritardo
    String r;
    for (int i = 10; i > 0 && r.length() == 0; i -= 2) {
      Serial.printf("\nScrivi 'config' e premi Invio per cambiare le impostazioni (%d secondi)\n", i);
      r = leggiRiga(2000);
      r.trim();
    }
    if (!r.equalsIgnoreCase("config")) return;
  } else {
    Serial.println("\n*** Configurazione mancante: scrivila qui sotto ***");
  }

  aiutoConfig();
  mostraConfig();
  Preferences p;
  p.begin("config", false);
  while (true) {
    Serial.print("\n> ");
    String riga = leggiRiga(600000);   // 10 minuti
    if (riga.length() == 0) continue;
    String pulita = riga;
    pulita.trim();
    String minuscola = pulita;
    minuscola.toLowerCase();
    if (minuscola.startsWith("password")) Serial.println("password=********");   // non la ripete in chiaro
    else                                  Serial.println(pulita);

    if (pulita.equalsIgnoreCase("fine")) {
      if (configCompleta()) break;
      Serial.println("Prima completa ssid, token, chat e nome.");
      continue;
    }
    if (pulita.equalsIgnoreCase("mostra"))  { mostraConfig(); continue; }
    if (pulita.equalsIgnoreCase("aiuto"))   { aiutoConfig();  continue; }
    if (pulita.equalsIgnoreCase("lettura")) {
      int g = leggiGrezzo();
      Serial.printf("Lettura sensore: %d (%d%%)\n", g, percentuale(g));
      if (USA_BATTERIA) Serial.printf("Batteria: %.2f V\n", leggiBatteria());
      continue;
    }
    if (pulita.equalsIgnoreCase("mattino")) {
      if (!configCompleta()) { Serial.println("Prima completa ssid, token, chat e nome."); continue; }
      sincronizzaOra();
      int g = leggiGrezzo();
      Serial.println(inviaMessaggioMattino(g, percentuale(g)) ? "Messaggio mandato." : "Messaggio NON mandato.");
      spegniWiFi();
      continue;
    }
    if (pulita.equalsIgnoreCase("cancella")) {
      p.clear();
      p.end();
      caricaConfig();
      p.begin("config", false);
      Serial.println("Impostazioni cancellate.");
      continue;
    }

    int uguale = riga.indexOf('=');
    if (uguale < 0) { Serial.println("Non capisco: usa chiave=valore oppure 'aiuto'."); continue; }
    String chiave = riga.substring(0, uguale);
    chiave.trim();
    chiave.toLowerCase();
    String valore = riga.substring(uguale + 1);   // la password può contenere spazi e '='

    if      (chiave == "ssid")           p.putString("ssid", valore);
    else if (chiave == "password")       p.putString("pass", valore);
    else if (chiave == "token")          { valore.trim(); p.putString("token", valore); }
    else if (chiave == "chat")           { valore.trim(); p.putString("chat", valore); }
    else if (chiave == "nome")           { valore.trim(); p.putString("nome", valore); }
    else if (chiave == "secco")          p.putInt("secco", valore.toInt());
    else if (chiave == "bagnato")        p.putInt("bagnato", valore.toInt());
    else if (chiave == "soglia_secco")   p.putInt("s_secco", valore.toInt());
    else if (chiave == "soglia_bagnato") p.putInt("s_bagnato", valore.toInt());
    else { Serial.println("Chiave sconosciuta: " + chiave); continue; }

    p.end();
    caricaConfig();
    p.begin("config", false);
    Serial.println("Salvato.");
  }
  p.end();
  Serial.println("Configurazione salvata. Avvio...");
}

// ---------- Protezione aggiornamenti ----------
// Dopo un aggiornamento, il firmware nuovo deve dimostrare di riuscire a
// parlare con Telegram. Se non ci riesce, la scheda torna alla versione
// precedente. (Funziona solo se il core ESP32 ha il "rollback" attivo;
// altrimenti queste funzioni non fanno niente.)

bool verifyRollbackLater() {
  return true;    // la conferma la diamo noi, dopo il messaggio "è online"
}

void confermaFirmware(bool funziona) {
  const esp_partition_t* attuale = esp_ota_get_running_partition();
  esp_ota_img_states_t statoOta;
  if (esp_ota_get_state_partition(attuale, &statoOta) != ESP_OK) return;
  if (statoOta != ESP_OTA_IMG_PENDING_VERIFY) return;   // non è un firmware appena installato
  if (funziona) {
    Serial.println("Firmware nuovo confermato");
    esp_ota_mark_app_valid_cancel_rollback();
  } else {
    Serial.println("Il firmware nuovo non raggiunge Telegram: torno alla versione precedente");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Arriva qui solo se il ritorno non è riuscito: mai dormire con un firmware
    // non confermato, si riavvia e ci pensa il bootloader
    delay(200);
    ESP.restart();
  }
}

// ---------- Avvio ----------

void vaiADormire() {
  confermaFirmware(false);   // non fa niente se il firmware è già confermato
  spegniWiFi();
  Serial.println("Vado a dormire");
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)MINUTI_SONNO * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  bool risveglio = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
  if (DEBUG) {
    // aspetta fino a 10 s che il monitor seriale si colleghi
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 10000) delay(10);
    delay(500);
  }
  Serial.printf("\n--- Sensore pianta avviato (firmware v%s) ---\n", testoVersione(VERSIONE_FIRMWARE).c_str());
  impostaFuso();
  caricaConfig();
  caricaStato();

  // Solo all'accensione (non ai risvegli): possibilità di configurare dal monitor seriale
  if (!risveglio) configurazioneSeriale();

  if (!configCompleta()) {
    // Senza Wi-Fi e Telegram non può fare niente: dorme finché non premi RST
    // (un firmware appena aggiornato che non trova la configurazione torna indietro)
    confermaFirmware(false);
    Serial.println("Configurazione mancante: collega l'USB, apri il monitor seriale e premi RST.");
    Serial.flush();
    esp_deep_sleep_start();
  }

  // Solo all'accensione (non ai risvegli): legge l'ora e manda il messaggio di prova
  if (!risveglio) {
    bool oraOk = sincronizzaOra();
    String rigaOra = "🕗 Ora non disponibile, riprovo più tardi";
    struct tm t;
    if (oraOk && oraLocale(t)) {
      char buf[6];
      strftime(buf, sizeof(buf), "%H:%M", &t);
      rigaOra = String("🕗 Ora: ") + buf;
      // se e' gia' passata l'ora dello stato, il primo arriva domattina
      if (t.tm_hour >= ORA_STATUS) st.giornoUltimoStatus = codiceGiorno(t);
    }

    int g = leggiGrezzo();
    String msg = String("🌱 <b>") + nomeHtml() + "</b> è online!\n" +
                 "💧 Umidità del terreno: <b>" + percentuale(g) + "%</b>\n" +
                 "📟 Lettura sensore: " + g +
                 rigaBatteria() + "\n" +
                 rigaOra + "\n" +
                 "⏰ Controllo ogni " + MINUTI_SONNO + " minuti\n" +
                 "☀️ Stato e grafico ogni mattina dopo le " + ORA_STATUS + ":00\n" +
                 "⚙️ Firmware v" + testoVersione(VERSIONE_FIRMWARE) + (VERSIONE_FIRMWARE == 0 ? " (compilato a mano)" : "");
    confermaFirmware(inviaTelegram(msg));
    salvaStato();
  }

  if (DEBUG) { spegniWiFi(); return; }   // in modalita prove si continua nel loop()

  controlla();
  vaiADormire();
}

// Usato solo con DEBUG true
void loop() {
  static bool primo = true;
  static unsigned long ultimoControllo = 0;

  if (primo || millis() - ultimoControllo >= (unsigned long)MINUTI_SONNO * 60000UL) {
    controlla();
    spegniWiFi();
    ultimoControllo = millis();
    primo = false;
  } else {
    int g = leggiGrezzo();
    Serial.printf("Lettura: %d (%d%%)\n", g, percentuale(g));
    if (USA_BATTERIA) {
      float v = leggiBatteria();
      Serial.printf("Batteria: %.2f V (%d%%)\n", v, percentualeBatteria(v));
    }
  }
  delay(2000);
}

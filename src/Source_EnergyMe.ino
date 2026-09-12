// ****************************************************************
// * Client EnergyMe (https://www.energyme.net/) - API REST       *
// * GET /api/v1/ade7953/meter-values, authentification HTTP      *
// * Digest (RFC 2617, qop=auth, MD5). Adresse IP dans RMSextIP    *
// * (ou EnergyMeIP_T pour un 2e boîtier dédié au Triac).          *
// * Le challenge Digest (realm/nonce/opaque) est mis en cache par *
// * appareil : une seule requête réseau par lecture en régime     *
// * normal ("nc" incrémenté), au lieu de 2 systématiquement - very*
// * important sur ce chip mono-coeur pour ne pas geler le WiFi/   *
// * serveur web pendant la lecture (voir 1ère version, trop lente)*
// ****************************************************************
#include <MD5Builder.h>
#include "EnergyMe.h"

String MD5Hex(String s) {
  MD5Builder md5;
  md5.begin();
  md5.add((const uint8_t *)s.c_str(), s.length());
  md5.calculate();
  return md5.toString();
}

// Extrait la valeur d'un champ "champ=valeur" ou "champ=\"valeur\"" d'une ligne d'entête HTTP
String ExtraitChampAuth(String ligne, String champ) {
  int p = ligne.indexOf(champ + "=");
  if (p < 0) return "";
  p += champ.length() + 1;
  if (ligne.charAt(p) == '"') {
    p++;
    int q = ligne.indexOf('"', p);
    if (q < 0) return "";
    return ligne.substring(p, q);
  }
  int q = ligne.indexOf(',', p);
  if (q < 0) q = ligne.length();
  String v = ligne.substring(p, q);
  v.trim();
  return v;
}

CacheDigestEnergyMe CacheEnergyMePrincipal;  //Boîtier de la Source principale (RMSextIP)
CacheDigestEnergyMe CacheEnergyMeTriac;      //2e boîtier dédié au Triac (EnergyMeIP_T), si configuré

// Envoie une requête GET (authentifiée ou non selon enteteAuth) et renvoie la réponse complète
// (entêtes + corps), avec des timeouts courts adaptés à un appareil sur le réseau local.
String RequeteHttpBrute(String host, String uri, String enteteAuth) {
  WiFiClient client;
  if (!client.connect(host.c_str(), 80, 1500)) return "";
  String requete = String("GET ") + uri + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
  if (enteteAuth.length() > 0) requete += "Authorization: " + enteteAuth + "\r\n";
  requete += "\r\n";
  client.print(requete);
  unsigned long timeout = millis();
  while (client.available() == 0 && client.connected() && (millis() - timeout < 1500)) {
    ;
  }
  String reponse = "";
  timeout = millis();
  while ((client.available() || client.connected()) && (millis() - timeout < 1500)) {
    if (client.available()) {
      reponse += client.readStringUntil('\r');
      timeout = millis();
    }
  }
  client.stop();
  return reponse;
}

// Récupère un nouveau challenge Digest (realm/nonce/opaque/qop) depuis l'appareil et le stocke
// dans le cache fourni (compteur nc réinitialisé).
bool RafraichitChallengeEnergyMe(CacheDigestEnergyMe &cache, String host, String uri) {
  String reponse = RequeteHttpBrute(host, uri, "");
  int p = reponse.indexOf("WWW-Authenticate:");
  if (p < 0) {
    StockMessage("EnergyMe: pas de challenge Digest reçu : " + host);
    return false;
  }
  String ligneAuth = reponse.substring(p);
  int finLigne = ligneAuth.indexOf("\r");
  if (finLigne > 0) ligneAuth = ligneAuth.substring(0, finLigne);

  cache.realm = ExtraitChampAuth(ligneAuth, "realm");
  cache.nonce = ExtraitChampAuth(ligneAuth, "nonce");
  cache.opaque = ExtraitChampAuth(ligneAuth, "opaque");
  cache.qop = ExtraitChampAuth(ligneAuth, "qop");
  cache.host = host;
  cache.nc = 0;
  if (cache.realm == "" || cache.nonce == "") {
    StockMessage("EnergyMe: challenge Digest incomplet : " + host);
    return false;
  }
  return true;
}

// Requête GET authentifiée en HTTP Digest, avec cache du challenge par appareil : une seule requête
// réseau en régime normal, deux seulement au tout premier appel pour cet appareil ou si le nonce en
// cache est refusé (périmé). Renvoie le corps JSON de la réponse, ou "" en cas d'échec.
String RequeteEnergyMe(CacheDigestEnergyMe &cache, String host, String utilisateur, String motDePasse, String uri) {
  if (cache.host != host || cache.nonce == "") {
    if (!RafraichitChallengeEnergyMe(cache, host, uri)) return "";
  }

  for (int essai = 0; essai < 2; essai++) {
    cache.nc++;
    char ncHex[9];
    snprintf(ncHex, sizeof(ncHex), "%08lx", cache.nc);
    String cnonce = String(millis(), HEX) + String(random(0, 0xFFFF), HEX);
    String HA1 = MD5Hex(utilisateur + ":" + cache.realm + ":" + motDePasse);
    String HA2 = MD5Hex(String("GET:") + uri);
    String reponseDigest = MD5Hex(HA1 + ":" + cache.nonce + ":" + String(ncHex) + ":" + cnonce + ":" + cache.qop + ":" + HA2);
    String authHeader = "Digest username=\"" + utilisateur + "\", realm=\"" + cache.realm + "\", nonce=\"" + cache.nonce
                         + "\", uri=\"" + uri + "\", qop=" + cache.qop + ", nc=" + String(ncHex) + ", cnonce=\"" + cnonce
                         + "\", response=\"" + reponseDigest + "\", opaque=\"" + cache.opaque + "\"";

    String reponse = RequeteHttpBrute(host, uri, authHeader);
    if (reponse.length() == 0 || reponse.indexOf("WWW-Authenticate:") >= 0) {
      //Nonce périmé/refusé (ou échec réseau) : on renouvelle le challenge et on retente une fois
      if (essai == 0 && RafraichitChallengeEnergyMe(cache, host, uri)) continue;
      StockMessage("EnergyMe: authentification refusée : " + host);
      return "";
    }
    int p2 = reponse.indexOf("[");
    if (p2 < 0) p2 = reponse.indexOf("{");
    if (p2 < 0) {
      StockMessage("EnergyMe: réponse invalide : " + host);
      return "";
    }
    return reponse.substring(p2);
  }
  return "";
}

// Extrait le canal "label" indiqué (ou tout le message si label vide) et alimente les variables de
// puissance/énergie fournies par référence, en miroir de ce qui existe pour Maison ("_M") et Triac
// ("_T") ailleurs dans le projet.
void ExtraitCanalEnergyMe(String data, String label, float &Tension, float &Intensite, float &PS_inst, float &PI_inst, float &PVAS_inst, float &PVAI_inst, long &EnergieSoutiree, long &EnergieInjectee) {
  String bloc = data;
  if (label.length() > 0) {
    int pos = TrouveBloc("label", label, data);
    if (pos < 0) {
      TelnetPrintln("EnergyMe: canal \"" + label + "\" introuvable dans la réponse");
      return;
    }
    bloc = data.substring(pos);
  }
  float V = ValJson("voltage", bloc);
  float I = ValJson("current", bloc);
  float Pact = ValJson("activePower", bloc);
  float Papp = ValJson("apparentPower", bloc);
  Intensite = I;
  if (V > 0) Tension = V;
  if (Pact >= 0) {
    PS_inst = Pact;
    PI_inst = 0;
    PVAS_inst = Papp;
    PVAI_inst = 0;
  } else {
    PS_inst = 0;
    PI_inst = -Pact;
    PVAS_inst = 0;
    PVAI_inst = Papp;
  }
  if (bloc.indexOf("activeEnergyImported") > 0) EnergieSoutiree = long(ValJson("activeEnergyImported", bloc));
  if (bloc.indexOf("activeEnergyExported") > 0) EnergieInjectee = long(ValJson("activeEnergyExported", bloc));
}

void LectureEnergyMe() {
  //Si le WiFi n'est pas connecté (ex: retombé en mode point d'accès), une tentative de connexion
  //vers une IP locale devenue injoignable peut bloquer bien plus longtemps que le timeout demandé
  //(cas observé du driver WiFi ESP32). On évite complètement d'essayer dans ce cas.
  if (WiFi.status() != WL_CONNECTED) return;
  String data = RequeteEnergyMe(CacheEnergyMePrincipal, IP2String(RMSextIP), EnergyMeUser, EnergyMePwd, "/api/v1/ade7953/meter-values");
  if (data.length() == 0) return;

  ExtraitCanalEnergyMe(data, LabelP, Tension_M, Intensite_M, PuissanceS_M_inst, PuissanceI_M_inst, PVAS_M_inst, PVAI_M_inst, Energie_M_Soutiree, Energie_M_Injectee);
  Pva_valide = true;
  EnergieActiveValide = true;

  //Triac sur le même boîtier, seulement si aucun 2e boîtier dédié (EnergyMeIP_T) n'est configuré
  if (LabelIT.length() > 0 && EnergyMeIP_T == 0) {
    ExtraitCanalEnergyMe(data, LabelIT, Tension_T, Intensite_T, PuissanceS_T_inst, PuissanceI_T_inst, PVAS_T_inst, PVAI_T_inst, Energie_T_Soutiree, Energie_T_Injectee);
  }

  filtre_puissance();
  PuissanceRecue = true;
  if (cptLEDyellow > 30) cptLEDyellow = 4;
}

// Mesure Triac depuis un 2e boîtier EnergyMe séparé (IP/identifiants propres, EnergyMeIP_T/
// EnergyMeUser_T/EnergyMePwd_T), indépendante de Source — appelée sur son propre timer.
void LectureEnergyMe_Triac() {
  if (WiFi.status() != WL_CONNECTED) return;
  String data = RequeteEnergyMe(CacheEnergyMeTriac, IP2String(EnergyMeIP_T), EnergyMeUser_T, EnergyMePwd_T, "/api/v1/ade7953/meter-values");
  if (data.length() == 0) return;
  ExtraitCanalEnergyMe(data, LabelIT, Tension_T, Intensite_T, PuissanceS_T_inst, PuissanceI_T_inst, PVAS_T_inst, PVAI_T_inst, Energie_T_Soutiree, Energie_T_Injectee);
  filtre_puissance();
}

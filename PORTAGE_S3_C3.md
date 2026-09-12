# Portage ESP32-S3 / ESP32-C3 et mesures MQTT multi-canaux

Ce document décrit les changements apportés dans ce fork (daniellouvel) par rapport au
projet [F1ATB/Solar-Router-F1ATB](https://github.com/F1ATB/Solar-Router-F1ATB) d'origine.
Il est séparé du `README.md` et du changelog du fichier principal pour ne pas interférer
avec le suivi de version de l'auteur d'origine — libre à lui de reprendre tout ou partie
de ce qui suit.

## 1. Contexte

Objectif : faire tourner ce firmware sur ESP32-S3 (cible finale) et ESP32-C3 SuperMini
(carte de test utilisée en attendant la S3). Le projet ciblait jusqu'ici uniquement
l'ESP32 classique (Wroom) et n'était pas structuré en projet PlatformIO.

## 2. Réorganisation en projet PlatformIO

- Tous les fichiers `.ino`/`.cpp`/`.h` déplacés dans `src/` (PlatformIO les concatène et
  génère les prototypes comme le fait l'IDE Arduino — l'ordre entre fichiers ne change
  rien).
- `platformio.ini` ajouté avec deux environnements :
  - `esp32s3_n16r8` — cible finale (ESP32-S3, 16MB flash, 8MB PSRAM octal), table de
    partitions dédiée `partitions_s3_16mb.csv` (6MB par slot OTA A/B).
  - `esp32c3_supermini` — carte de test (ESP32-C3 SuperMini, 4MB flash, sans écran),
    utilise la `partitions.csv` à la racine, adaptée en un seul slot d'appli (le firmware
    compilé, ~1.97MB, ne tient pas dans un schéma à 2 slots OTA de 1900K sur une puce
    4MB).
- `lib_ignore = OneWire` + `build_flags = -Isrc` : le projet embarque sa propre copie
  modifiée de `OneWire.cpp`/`.h` dans `src/` ; ces réglages évitent un conflit de
  symboles avec la copie que `DallasTemperature` installerait automatiquement, tout en
  lui permettant de retrouver la copie du projet.

## 3. Corrections nécessaires à la compilation sur S2/S3/C3

Ces adaptations n'affectent pas l'ESP32 classique (gardées derrière des `#if` ciblant le
chipset, ou par simple ajout de macros de repli).

- **SPI legacy** — `HSPI_HOST`/`VSPI_HOST` n'existent nativement que sur l'ESP32
  classique. Shim ajouté dans `EcranLCD.h` :
  ```cpp
  #ifndef HSPI_HOST
  #define HSPI_HOST SPI2_HOST
  #endif
  #ifndef VSPI_HOST
  #if SOC_SPI_PERIPH_NUM > 2
  #define VSPI_HOST SPI3_HOST
  #else
  #define VSPI_HOST SPI2_HOST  // C3 : un seul SPI usage général
  #endif
  #endif
  ```
- **EMAC / Ethernet (WT32-ETH01)** — le driver EMAC (`ETH_PHY_LAN8720`) n'existe que sur
  l'ESP32 classique (aucune puce S2/S3/C3 n'a de MAC Ethernet interne). La déclaration du
  driver et son initialisation sont maintenant sous `#if CONFIG_ETH_USE_ESP32_EMAC` dans
  `Solar_Router_V17_26.ino`.

## 4. Bug mémoire préexistant corrigé (indépendant du portage)

Dans `setup()` (`Solar_Router_V17_26.ino`), la boucle d'initialisation de
`RMS_NomEtat[]` contenait :

```cpp
for (int i = 0; i < LES_ROUTEURS_MAX; i++) {
    RMS_IP[i] = 0;
    RMS_NomEtat[LES_ROUTEURS_MAX] = "";   // <- bug : index fixe au lieu de [i]
    ...
```

`RMS_NomEtat` est un tableau de `LES_ROUTEURS_MAX` (8) `String` (indices 0-7). Cette
ligne écrivait hors-limites à l'indice 8 à chaque itération. Sur l'ESP32 classique, ce
memory-corruption bug restait invisible par chance de disposition mémoire. Sur l'ESP32-C3
(RISC-V), la même écriture corrompait `RMS_IP[3]`, provoquant des appels réseau bloquants
de plusieurs secondes par boucle (tentative de connexion à un "routeur RMS" fantôme à une
IP invalide), suffisamment pour perturber le point d'accès WiFi. Corrigé en `[i]`.

## 5. Défaut d'antenne connu — ESP32-C3 SuperMini (1ers lots)

Les premiers lots de cartes "ESP32-C3 SuperMini" ont une antenne mal adaptée qui empêche
l'association WiFi en pleine puissance d'émission (~20dBm) : le scan/RSSI fonctionne
(réception), mais l'association reste bloquée indéfiniment (émission). Signalé et discuté
dans la communauté (voir [forum Arduino](https://forum.arduino.cc/t/no-wifi-connect-with-esp32-c3-super-mini/1324046)).
Contournement appliqué, sans effet sur les autres cartes :

```cpp
#if CONFIG_IDF_TARGET_ESP32C3
WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
```

## 6. Nouvelles entrées ESP32_Type : cartes C3 SuperMini et S3 N16R8

Le choix de carte (`ESP32_Type`, page Paramètres) gagne deux nouvelles valeurs :

| Valeur | Carte | Profil GPIO Actions |
|---|---|---|
| 102 | ESP32-C3 SuperMini | `0,1,3,4,5,6,7,10,20,21` (hors GPIO2/8/9 strapping, 18/19 USB natif) |
| 103 | ESP32-S3 N16R8 | `1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,47` (hors GPIO0/3/45/46/48 strapping/LED, 19/20 USB, 26-37 flash+PSRAM octal, 43/44 UART0) |

Les broches déjà proposées pour le Triac (GPIO4/GPIO5) et la température DS18B20
(GPIO21) sont électriquement valides sur ces deux puces — aucune nouvelle option n'était
nécessaire pour ces menus, seulement pour la liste de GPIO génériques des Actions
(`handlePinsActionsJS()` dans `Server.ino`).

Ces deux valeurs sont traitées comme des cartes WiFi sans écran (comme "Wroom seul")
partout où le code distingue WiFi/Ethernet/écran par plage de valeurs numériques
(`ESP32_Type < 10 || == 101 || == 102 || == 103`).

## 7. Mesure Maison et/ou Triac via MQTT sur un message multi-canaux

Motivation : cette fonctionnalité a été ajoutée pour pouvoir exploiter les valeurs
remontées en MQTT par [EnergyMe](https://www.energyme.net/), un moniteur d'énergie
open-source multi-canaux. Comme d'autres capteurs multi-canaux (ex: Shelly Pro 3EM),
EnergyMe publie un **tableau JSON** avec plusieurs canaux nommés, par exemple :

```json
[
  {"index":0,"label":"General","role":"grid","data":{"voltage":228.6,"current":2.9,"activePower":508,"apparentPower":673,"activeEnergyImported":137131.4,"activeEnergyExported":5905.8}},
  {"index":1,"label":"Cumulus","role":"load","data":{"voltage":228.6,"current":0.6,"activePower":86,"apparentPower":138,"activeEnergyImported":2.7,"activeEnergyExported":0}}
]
```

Le mode `Source = MQTT` ne savait lire qu'un format simple `{"Pw":..., "Pva":..., "Pf":...}`
sur un seul topic, et uniquement pour la puissance Maison — pas de mesure séparée pour le
Triac (contrairement à UxIx2/UxIx3 qui, via un capteur externe dédié, fournissent les
deux). Deux nouveaux champs, indépendants l'un de l'autre :

- **`TopicP` / `LabelP`** (section Mesures de puissance) — puissance Maison. Si `LabelP`
  est vide : comportement d'origine (`{"Pw":...}`). Si renseigné : le message reçu sur
  `TopicP` est traité comme un tableau à plusieurs canaux, et le bloc dont `"label"` vaut
  `LabelP` est utilisé directement (`voltage`/`current`/`activePower`/`apparentPower`/
  `activeEnergyImported`/`activeEnergyExported`).
- **`TopicIT` / `LabelIT`** (section Routeur) — mesure Triac, **indépendante de `Source`**
  (fonctionne même si `Source` n'est pas MQTT, ex: Linky + Triac mesuré séparément par
  MQTT). Même mécanisme d'extraction que `TopicP`/`LabelP`.

`TopicP` et `TopicIT` peuvent pointer vers le **même topic** (deux labels différents dans
le même message, cas Shelly Pro 3EM ci-dessus) ou vers **deux topics distincts** (deux
appareils différents) — les deux abonnements et traitements sont indépendants.

### Détails d'implémentation

- `TrouveBloc(nomCle, valeurCherchee, Json)` (`Source_EnphaseEnvoy.ino`) — cherche, dans un
  message JSON à plusieurs objets similaires, le bloc dont la clé `nomCle` vaut
  `valeurCherchee`, tolère un espace optionnel après `:`. Renvoie la position juste après
  ce bloc, pour que les `ValJson(...)` suivants n'y lisent que les valeurs de ce bloc.
- Contrairement à l'intégration locale de puissance (`EASfloat`/`EAIfloat`, utilisée par
  UxI et par le format simple `{"Pw":...}`), les compteurs d'énergie en mode "label" sont
  lus **directement depuis l'appareil** (`activeEnergyImported`/`activeEnergyExported`),
  comme le fait déjà UxIx2 avec son capteur JSY externe — pas d'intégration locale
  approximative dépendant de la cadence de publication MQTT du capteur.
- Une fois `TopicIT`/`LabelIT` configurés, la mesure Triac est traitée **partout comme
  UxIx2** : graphique et tableau de la page d'accueil (variable JS `biSonde`), fichier CSV
  mensuel d'historique, découverte/publication Home Assistant (`Intensite_T`, `Tension_T`,
  etc.), page "données brutes".

## 8. Source "EnergyMe" : lecture directe en HTTP (sans MQTT)

Motivation : pouvoir lire les mêmes données [EnergyMe](https://www.energyme.net/) qu'en
§7, mais sans dépendre d'un broker MQTT — le routeur interroge directement l'API REST du
boîtier EnergyMe. Nouvelle valeur `Source = "EnergyMe"` (`src/Source_EnergyMe.ino`).

### Authentification HTTP Digest

L'API EnergyMe (`GET /api/v1/ade7953/meter-values`, [swagger.yaml](https://raw.githubusercontent.com/jibrilsharafi/EnergyMe-Home/main/source/resources/swagger.yaml))
est protégée par **HTTP Digest** (RFC 2617, MD5, `qop=auth`) dès qu'un mot de passe non
défaut est configuré sur l'appareil — pas de Basic, pas de cookie. `HTTPClient` du core
Arduino-ESP32 ne supporte que Basic ; l'implémentation est donc manuelle
(`MD5Builder` pour HA1/HA2/response) dans `Source_EnergyMe.ino`.

**Point important, découvert en test réel sur le C3** : refaire le handshake Digest complet
(2 requêtes réseau : la 1ère pour obtenir `realm`/`nonce`/`opaque` dans le 401, la 2nde pour
la requête authentifiée) à **chaque lecture** rendait le routeur inutilisable (serveur web
qui ne répond plus, boucle principale à plusieurs secondes voire dizaines de secondes). Le
challenge Digest est donc **mis en cache par appareil** (`struct CacheDigestEnergyMe` dans
`EnergyMe.h`, un compteur `nc` incrémenté à chaque requête) : une seule requête réseau par
lecture en régime normal, le handshake complet n'étant refait qu'au premier appel ou si le
nonce en cache est refusé (périmé). Le `struct` est dans son propre `.h` (et pas dans le
`.ino`) car un type personnalisé utilisé en paramètre de fonction dans un `.ino` casse la
génération automatique de prototypes de PlatformIO/Arduino s'il n'est pas déjà connu au
moment où le prototype est inséré en haut du fichier fusionné.

Autre garde-fou ajouté suite au même test : si le WiFi n'est pas connecté (ex: routeur
retombé en mode point d'accès), une tentative de connexion vers l'IP locale du boîtier peut
bloquer bien plus longtemps que le timeout demandé (comportement observé du driver WiFi
ESP32 dans cet état). `LectureEnergyMe()`/`LectureEnergyMe_Triac()` vérifient donc
`WiFi.status() == WL_CONNECTED` avant toute tentative.

### Champs de configuration

Réutilise `RMSextIP` (déjà utilisé par HomeWizard/ShellyEm/Enphase pour l'IP de
l'appareil) et `LabelP`/`LabelIT` (§7, même sémantique : nom du canal à extraire dans la
réponse). Nouveaux champs : `EnergyMeUser`/`EnergyMePwd` (identifiants du boîtier
principal). Tous regroupés dans la section "Mesures de puissance" de la page Paramètres,
Maison puis Triac à la suite (à l'origine dispersés entre "Mesures de puissance" et
"Routeur" lors des premiers ajouts — réorganisés pour rester lisibles).

### Triac sur un 2e boîtier EnergyMe indépendant

Comme pour `TopicIT` en MQTT (§7), le Triac peut venir d'un **appareil EnergyMe séparé**
avec ses propres identifiants : `EnergyMeIP_T`/`EnergyMeUser_T`/`EnergyMePwd_T`, lu sur son
propre timer (indépendant du rythme de lecture de la Source principale, et indépendant de
`Source` lui-même — fonctionne même si `Source` n'est pas `EnergyMe`). Utilise son propre
cache Digest (`CacheEnergyMeTriac`, distinct de `CacheEnergyMePrincipal`). Si
`EnergyMeIP_T` est laissé vide, le Triac est lu depuis le **même** boîtier que la Source
principale (une seule requête sert les deux canaux). La fonction `TriacIndependant()`
(`commonFx.ino`) centralise la condition "Triac disponible indépendamment de Source"
(`TopicIT` MQTT non vide, ou `LabelIT` non vide avec `Source == "EnergyMe"` ou
`EnergyMeIP_T` configuré) — utilisée partout où le Triac doit s'afficher/s'enregistrer
comme en UxIx2 (voir §7).

**Attention** : ne pas renseigner `EnergyMeIP_T` avec la **même** IP que `RMSextIP` avant
d'avoir réellement un 2e boîtier — cela fait interroger deux fois le même appareil en
parallèle (deux sessions Digest distinctes) pour rien, puisqu'une seule requête sur le
boîtier principal donne déjà les deux canaux.

## 9. Fichiers modifiés

`platformio.ini`, `partitions.csv`, `partitions_s3_16mb.csv` (nouveau), `.gitignore`
(nouveau), `src/EcranLCD.h`, `src/Solar_Router_V17_26.ino`, `src/EnvoiMQTT.ino`,
`src/Source_MQTT.ino`, `src/Source_EnergyMe.ino` (nouveau), `src/EnergyMe.h` (nouveau),
`src/Source_EnphaseEnvoy.ino`, `src/Server.ino`, `src/Stockage.ino`, `src/commonFx.ino`,
`src/PagePara.h`, `src/JS_Para.h`, `src/JS_Brute.h`, `src/EcranLED.ino`.

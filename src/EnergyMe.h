#ifndef EnergyMe_h
#define EnergyMe_h
// Cache du challenge HTTP Digest pour un appareil EnergyMe (voir Source_EnergyMe.ino).
// Défini dans un .h à part : un struct utilisé en paramètre de fonction dans un .ino casse la
// génération automatique de prototypes de PlatformIO/Arduino si le type n'est pas déjà connu.
struct CacheDigestEnergyMe {
  String host = "";
  String realm = "", nonce = "", opaque = "", qop = "";
  unsigned long nc = 0;
};
#endif

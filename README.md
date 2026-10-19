# ESP32-S3 Freenove CAM – IP Camera

Progetto sperimentale per utilizzare una scheda ESP32-S3 Freenove CAM come telecamera IP nella rete locale, con streaming MJPEG, acquisizione di foto e registrazione video su microSD.

## ⚠️ Disclaimer

Questo codice è stato generato con l'assistenza dell'intelligenza artificiale (AI) ed è destinato esclusivamente a scopi di test e sperimentazione.

**Il codice potrebbe contenere bug, vulnerabilità e gravi problemi di sicurezza.** Non è stato sottoposto a un audit di sicurezza professionale e non deve essere considerato sicuro per un utilizzo in produzione.

In particolare:

- Il server web non implementa un sistema di autenticazione.
- L'esposizione del dispositivo a Internet, ad esempio tramite **port forwarding** sul router, può consentire a terzi di accedere alla telecamera e alle sue funzionalità.
- Non esporre il dispositivo a reti pubbliche o a Internet senza aver prima implementato e verificato adeguate misure di sicurezza.

Utilizzare il progetto esclusivamente in ambienti controllati e su reti fidate.

L'autore non garantisce la sicurezza o il corretto funzionamento del codice e declina ogni responsabilità per eventuali danni derivanti dal suo utilizzo.

## Funzionalità

- Streaming video MJPEG in rete locale.
- Controllo dei parametri della telecamera tramite interfaccia web.
- Acquisizione di foto e registrazione video su microSD.
- Galleria web con download ed eliminazione dei file.

## Requisiti

- Scheda ESP32-S3 Freenove CAM compatibile.
- Arduino IDE con ESP32 core di Espressif.
- Scheda microSD (opzionale).

## Configurazione

1. Scaricare il [codice del progetto](./source/ESP32-S3-WROOM_WiFi-Camera.ino)
2. Aprire il progetto nell'Arduino IDE.
3. Inserire le proprie credenziali Wi-Fi nel codice.
4. Selezionare la scheda e le impostazioni hardware appropriate.
5. Compilare e caricare il firmware.
6. Aprire il Serial Monitor per individuare l'indirizzo IP e le porte utilizzate.

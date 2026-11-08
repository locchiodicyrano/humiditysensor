# Sensore di umidità per piante

ESP32-C3 SuperMini + sensore capacitivo HW-390 + batteria 18650.
Misura l'umidità della terra, avvisa su Telegram e si aggiorna da solo via Wi-Fi.

## Dove stanno password e token

**Non nel codice.** Wi-Fi, token del bot, chat e nome della pianta si scrivono
una volta sola dal monitor seriale e restano salvati nella memoria della scheda,
anche dopo gli aggiornamenti. Così il repository e il file `firmware.bin` possono
essere pubblici senza mostrare niente di privato.

## Prima installazione (con il cavo USB, una volta sola per scheda)

1. Arduino IDE: installa la libreria **ArduinoJson** versione 7 (Strumenti > Gestisci librerie).
2. Impostazioni della scheda:
   - Scheda: **ESP32C3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**
   - Flash Mode: **DIO**
3. Apri `sensore_pianta/sensore_pianta.ino`. `REPO_GITHUB` è già impostato su
   `locchiodicyrano/humiditysensor`, che deve restare **pubblico** per gli aggiornamenti.
4. Carica lo sketch e apri il monitor seriale (115200, fine riga **Nuova riga**).
5. Scrivi una riga alla volta e premi Invio:
   ```
   ssid=NomeRete
   password=PasswordWiFi
   token=TokenDelBot
   chat=IdDellaChat
   nome=Marble Queen Photos
   fine
   ```
   Con `lettura` vedi il valore del sensore (utile per la taratura),
   con `secco=...` e `bagnato=...` cambi la taratura, con `mostra` rivedi tutto.
6. Dopo il messaggio "è online" la scheda va in uso normale. Il mattino dopo
   installa da sola l'ultima versione pubblicata su GitHub.
   Con `mattino` puoi farti mandare subito il messaggio del mattino, per provarlo.

Per cambiare le impostazioni in seguito: collega l'USB, apri il monitor seriale,
premi RST e scrivi `config` entro 10 secondi.

## Pubblicare una nuova versione

```
git add .
git commit -m "descrizione della modifica"
git push
git tag v2          # numero sempre più alto: v2, v3, v4...
git push --tags
```

GitHub Actions compila e crea la Release (scheda **Actions** per vedere come va).
Le schede la installano al controllo del mattino. Non aggiornano se la batteria
è sotto 3,7 V, né quando sono in modalità prove (`DEBUG true`).
Attenzione: anche una versione caricata a mano via USB viene sostituita il mattino
dopo da quella di GitHub (se non è in modalità prove).

Ogni push senza tag fa solo una prova di compilazione: se diventa rossa c'è un errore.

## Messaggio del mattino

Ogni mattina dopo le 8 arriva un solo messaggio con:
- umidità e batteria di adesso;
- il grafico di umidità e batteria degli ultimi 7 giorni (fatto con quickchart.io);
- minimo, massimo e media dell'umidità, annaffiature rilevate;
- consumo della batteria al giorno e autonomia stimata, calcolati sulle misure
  fatte dopo l'ultima ricarica (almeno un giorno di dati).

Se quickchart.io non risponde, arriva lo stesso messaggio senza grafico.

**Niente comandi Telegram:** per risparmiare batteria la scheda accende il Wi-Fi
solo quando deve mandare un messaggio, quindi non legge quello che le scrivi.

## Se un aggiornamento va male

Dopo l'installazione il firmware nuovo deve riuscire a mandare il messaggio
"è online". Se non ci riesce, la scheda torna alla versione precedente (il core
3.3.12 ha questa funzione attiva) e riproverà il mattino dopo. Se la scheda resta bloccata, serve il
cavo USB: tieni premuto BOOT, premi e rilascia RST, rilascia BOOT e ricarica lo sketch.

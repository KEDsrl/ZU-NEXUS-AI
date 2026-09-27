# media/ — video di fallback di dxpose-qt

Copiare qui `dron_720p.y4m`. Se presente, il pacchetto `dxpose-qt` lo installa in

    /usr/share/dxpose-qt/media/dron_720p.y4m

e l'app lo usa al posto della camera quando:

- la catena video non viene rilevata o non si configura all'avvio;
- la pipeline della camera va in errore (v4l2src, not-negotiated, STREAMON...);
- la camera smette di produrre frame (4 s) o non ne produce entro 20 s dall'avvio.

Dal fallback si torna alla camera con il pulsante **Retry camera** nel pannello.

Il formato y4m non è compresso (720p30 ≈ 41 MB/s): tenere il video corto oppure
aumentare `BR2_TARGET_ROOTFS_EXT2_SIZE` nel defconfig.

Dopo aver sostituito il video:

    ./build.sh dxpose-qt-rebuild && ./build.sh

## logos/ — loghi del pannello

Tutti i `.png` presenti in `media/logos/` vengono installati in
`/usr/share/dxpose-qt/logos/` (la cartella nel rootfs viene prima svuotata).
L'app mostra solo i loghi che trova, in ordine alfabetico del nome file,
tutti alla stessa altezza e su una sola riga: 34 px se c'e' spazio, altrimenti
l'altezza si riduce in base al numero di loghi (con 4 loghi: circa 30 px).
Per aggiungerne o toglierne uno basta modificare questa cartella e poi:

    ./build.sh dxpose-qt-rebuild && ./build.sh

Sul target si possono anche cancellare/aggiungere file direttamente in
`/usr/share/dxpose-qt/logos/` e riavviare l'app (`systemctl restart dxpose-qt`).
Consigliati: sfondo trasparente, ritagliati sul contenuto, altezza >= 68 px.

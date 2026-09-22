# AP16 — WebRTC-Browserkompatibilität

Batchlauf 2026-09-23. Code committet als `37d1619` (Betreff
„choose H264 in the offer's preference order"), CI grün.

---

## Der gemeldete Befund, ehrlich eingeordnet

> `offer rejected: no H264 packetization-mode=1 video`

**Mit Chrome 153 und Edge 153 ist das nicht reproduzierbar.** Beide Browser
bieten vier H264-Nutzlasten in Modus 1 an; der Parser akzeptiert den Offer und
antwortet. Die Meldung stammt also entweder von einem Browser, der hier nicht
installiert ist (Firefox ohne H264-Decoder ist der naheliegende Kandidat), oder
aus einer früheren Codefassung. Ich habe sie nicht gesehen und behaupte keine
Zuordnung.

Die Untersuchung hat stattdessen **einen anderen, echten Defekt** freigelegt —
einen, den kein Testoffer je zeigen konnte.

## Wie gemessen wurde

Kein handgeschriebener Offer, sondern ein echter:

```
chrome.exe --headless=new --dump-dom --virtual-time-budget=8000 file:///.../offer.html
```

und in der Seite exakt das, was `preview-webrtc.js` baut:

```js
const pc = new RTCPeerConnection({ iceServers: [] });
pc.addTransceiver('video', { direction: 'recvonly' });
pc.addTransceiver('audio', { direction: 'recvonly' });
await pc.createOffer();
```

Chrome 153.0.8010.53 und Edge 153.0.4234.48, beide Windows 11. Der Offer liegt
wortgetreu in `tests/sdp_offers.hpp`. Edge unterscheidet sich von Chrome **nur**
in `o=`, `ice-ufrag`, `ice-pwd` und dem DTLS-Fingerprint (per `diff` geprüft) —
die Codec-Abschnitte sind bytegleich, deshalb ist er nicht zweimal gespeichert.
**Firefox ist auf dieser Maschine nicht installiert**; es wird kein
Firefox-Offer behauptet und keiner erfunden.

## Der Defekt

Chrome bietet vier H264-Nutzlasten mit `packetization-mode=1` an, in dieser
Reihenfolge in der m-Zeile:

| Reihenfolge | pt | profile-level-id | bedeutet |
|---|---|---|---|
| 1. | **102** | `42001f` | Constrained Baseline 3.1 |
| 2. | 108 | `42e01f` | Baseline 3.1 |
| 3. | 116 | `4d001f` | Main 3.1 |
| 4. | **41** | `f4001f` | High 4:4:4 Predictive 3.1 |

`parse_offer` suchte in einer `std::map<int, …>` — also in **Nummern**-, nicht
in **Präferenz**-Reihenfolge. 41 ist die kleinste Zahl. Machino antwortete
damit mit der **letzten** Wahl des Browsers, und zwar mit einem Profil, das ein
Chroma-Format nennt, das diese Kamera gar nicht erzeugt:

```
$ probe chrome.sdp          (alter Stand)
  [0] kind=video mid=0 h264_pt=41 plid=f4001f
```

Die Kamera sendet in Wahrheit `avc1.640033` — **High 4:2:0, Level 5.1**, für
MAIN (1920×1080) wie für SUB (640×360), direkt aus dem Live-Init-Segment
abgelesen. Dass es trotzdem funktioniert und hardwareabgenommen ist, liegt
allein daran, dass Chromes Decoder das Etikett ignoriert. Das ist Glück, keine
Eigenschaft.

**Warum kein Test das gefunden hat:** die bisherigen Offers im Testbestand
boten *eine* H264-Nutzlast an. Bei einer Nutzlast sind Nummernreihenfolge und
Präferenzreihenfolge dasselbe, und der Unterschied kann nicht sichtbar werden.
Genau dafür liegt jetzt ein echter Browseroffer im Repo.

## Behoben

```
$ probe chrome.sdp          (neuer Stand)
  [0] kind=video mid=0 pts=34 h264_pt=102 plid=42001f
$ probe edge.sdp
  [0] kind=video mid=0 pts=34 h264_pt=102 plid=42001f
```

1. **Auswahl in Offer-Reihenfolge.** Die m-Zeile ist die Präferenzliste des
   Browsers und der einzige Ort, an dem diese Reihenfolge überhaupt existiert;
   sie wird jetzt als `OfferMedia::pts` behalten und ausgewertet.

2. **Exakte Profilübereinstimmung schlägt Präferenz.** Bietet der Offer genau
   das Profil an, das der Encoder wirklich liefert, wird es genommen — dann
   *benennt* die Antwort den Strom, statt ihn zu nähern. Das Level gehört
   ausdrücklich **nicht** zum Vergleich; dafür gibt es
   `level-asymmetry-allowed`. Eine Kamera mit `4d0033` findet ihr `4d001f`.

   Woher das Profil kommt: aus dem SPS irgendeines Keyframes, der ohnehin durch
   einen Pump läuft. **Nie durch Warten.** Die RTSP-Seite darf für `DESCRIBE`
   bis zu drei Sekunden auf ein IDR blockieren; die Poll-Schleife darf das
   nicht, und eine verspätete SDP-Antwort ist schlechter als eine, die der
   Präferenz des Browsers folgt. Ist noch nichts bekannt, gilt eben die
   Präferenz — das ist ohnehin der richtige Standardfall.

   Für diese Kamera (`640033`) ändert das heute nichts: Chrome bietet High
   überhaupt nicht an, also bleibt es bei 102. Für Safari, das High anbietet,
   greift es.

3. **Zwei stille Strengheiten entfernt** — „keine unnötige Ablehnung eines
   kompatiblen H.264-Angebots" ist eine ausdrückliche AP16-Forderung:

   * Codec-Namen in `rtpmap` und Parameternamen in `fmtp` sind
     **case-insensitive** (RFC 4566, RFC 6184). `h264` oder
     `Packetization-Mode=1` wurden vorher abgelehnt. Das ist ein plausibler
     Ursprung genau der gemeldeten Fehlermeldung — belegen kann ich es nicht.
   * Eine Nutzlast, die per `rtpmap` beschrieben, in der m-Zeile aber
     vergessen wurde, ist streng genommen nicht angeboten. Sie wird jetzt
     trotzdem genommen, wenn in der Liste nichts passt. Diese Regel hat
     **sofort** einen bestehenden Test gerettet: die alte Testvorlage baute
     `m=video … 96` und beschrieb dann `a=rtpmap:102` — mein erster Entwurf
     lehnte sie ab. Der Test war schlampig, aber die Toleranz ist richtig.

4. **Das Annahme-Log nennt beide Profile:**

   ```
   offer accepted: pt=102 profile 42001f (camera sends 640033) candidate 192.168.1.10:40000
   ```

   Wenn die auseinandergehen, dekodiert der Browser einen Strom unter einem
   Etikett, das er selbst gewählt hat. Bei grünem, klotzigem oder fehlendem
   Bild ist das die erste Zeile, die man liest.

## Was ausdrücklich **nicht** angefasst wurde

ICE-lite, STUN, DTLS, SRTP, RTP/RTCP, PLI → IDR: unverändert. AP16 verlangt
das, solange kein konkreter Fehler vorliegt, und es lag keiner vor.

Ebenso **nicht** gemacht: das eigene Level (5.1) in die Antwort schreiben,
obwohl `level-asymmetry-allowed=1` das erlauben würde. Der Browser hat 3.1
genannt als das, was er zu empfangen bereit ist; ihm ungefragt 5.1 anzukündigen
ist der riskante Zug, und es gibt keinen Beleg, dass irgendein Browser ihn
braucht. Die Antwort spiegelt weiterhin die angebotene `profile-level-id`.

## MAIN und SUB

Beide Einheiten melden `avc1.640033` (live abgelesen: MAIN 1920×1080, SUB
640×360). Die Aushandlung ist damit für beide identisch, und der Profilcache
ist pro Einheit geführt — `/ws/webrtc?stream=1` bekommt das SUB-Profil, nicht
das von MAIN.

## Tests

`run_webrtc_ap16_tests()`, acht Blöcke gegen den echten Offer: Auswahl in
Präferenzreihenfolge (102, nicht 41), exakte Profiltreffer mit und ohne
Level-Unterschied, der Rückfall bei nicht angebotenem Profil, der vollständige
Antwort-Aufbau (BUNDLE nur für die bediente Sektion, `a=inactive` für Audio,
`setup:passive`, `sendonly`, `nack pli`) und ausdrücklich die Zusicherung, dass
`41` und `f4001f` **nirgends** mehr in der Antwort stehen; dazu blanke LFs,
Groß-/Kleinschreibung, beide Ablehnungsarten und die tolerante Rückfallregel.

2497 Hosttests, 0 failed. CI grün, MIPS-Crossbuild grün.

## Stand gegen das Regression-Gate

```
WebRTC MAIN ~50-100 ms                       PENDING_PHYSICAL
0 unerklärte DTLS/SRTP-Fehler                PENDING_PHYSICAL
PLI -> IDR funktioniert                      PENDING_PHYSICAL (Code unberührt)
kein Verlust der bestehenden Chrome-Kompat.  PENDING_PHYSICAL
```

**Das muss deutlich gesagt werden:** diese Änderung verschiebt die
ausgehandelte Nutzlast auf einem Pfad, der bereits hardwareabgenommen ist —
Chrome bekommt künftig pt 102 statt pt 41. Die Begründung ist stark (es ist
Chromes eigene erste Wahl, und es ist das, was jede andere WebRTC-Gegenstelle
aushandelt), und das Risiko ist gering, aber **auf Hardware nachgewiesen ist es
nicht**. Ein Daemon-Neustart ist dafür nötig, und ein Warmstart ist der
dokumentierte Hardlock-Auslöser.

Sollte WebRTC nach dem nächsten Kaltstart nicht mehr spielen, ist dies die
erste Änderung, die zurückzudrehen ist — sie ist in einem einzigen Commit
isoliert, und das Annahme-Log nennt die gewählte Nutzlast im Klartext.

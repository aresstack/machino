# EC200A am T40: die reale USB-Karte

> **Status: NICHT GEMESSEN (PENDING_PHYSICAL).**
>
> Am USB-Port der Kamera haengt derzeit der AIC8800-WLAN-Adapter, und es ist
> niemand da, der umstecken und den aktiv versorgten Hub anschliessen koennte.
> Diese Datei enthaelt deshalb noch keine Messwerte, sondern das Verfahren --
> und sie bleibt ausdruecklich leer, statt die Erwartung aus dem
> Referenzprojekt als Ergebnis hinzuschreiben.
>
> Erwartet wird (aus `Miguel0888/quectel-ec200a-eu`, README.md Zeile 7):
>
>     MI_00  Netzwerk (ECM bzw. RNDIS)
>     MI_02  DIAG
>     MI_03  AT
>     MI_04  Modem / PPP
>
> Wenn die Kamera etwas anderes zeigt, gewinnt die Kamera.

## Vor dem Anstecken

Das EC200A wird NICHT aus dem Kameraport gespeist. Ein LTE-Modul zieht im
Sendebetrieb Stromspitzen, die diese Schiene nicht vorgesehen hat; das
Vorprojekt arbeitet aus genau diesem Grund mit einem aktiven Hub. Dass der
AIC8800 am Port laeuft, beweist nur, dass die Schiene HS-USB kann.

    T40-USB-Port  ->  aktiver, extern versorgter Hub  ->  EC200A

Und: WLAN bleibt aus. Es gibt einen Port, und zwei Treiber, die ihn
gleichzeitig wollen, sind kein Test, sondern ein Durcheinander.

    echo off > /etc/machino/wifi-role        # Supervisor gibt wlan0 frei
    /etc/init.d/S42wifi stop

## Aufnehmen

Alles davon gehoert in diese Datei, nicht nur die VID:PID.

```sh
# 1. Sieht der Bus das Geraet ueberhaupt?
cat /sys/kernel/debug/usb/devices          # T:/P:/I:-Zeilen, alle Interfaces
ls /sys/bus/usb/devices/

# 2. Geraet und Interfaces einzeln
for d in /sys/bus/usb/devices/*/; do
    [ -r "$d/idVendor" ] || continue
    echo "$d $(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null)"
done
for i in /sys/bus/usb/devices/*:*/; do
    echo "$i if=$(cat $i/bInterfaceNumber) cls=$(cat $i/bInterfaceClass)" \
         "sub=$(cat $i/bInterfaceSubClass) prot=$(cat $i/bInterfaceProtocol)" \
         "eps=$(cat $i/bNumEndpoints) drv=$(basename $(readlink $i/driver 2>/dev/null) 2>/dev/null)"
done

# 3. Module laden (Reihenfolge zaehlt)
insmod /etc/machino/modules/usbserial.ko    # oder das des Images
insmod /etc/machino/modules/usb_wwan.ko
insmod /etc/machino/modules/option.ko
dmesg | tail -40

# 4. option kennt 0x6005 nicht -- Bring-up ueber den Kernelmechanismus
echo 2c7c 6005 > /sys/bus/usb-serial/drivers/option1/new_id
dmesg | tail -20
ls -l /sys/class/tty/ttyUSB*

# 5. Welcher tty haengt an welchem Interface? (genau das, was der Scanner tut)
for t in /sys/class/tty/ttyUSB*; do
    echo "$(basename $t) -> $(readlink $t)"
done
```

Schritt 5 ist der wichtige: er liefert dieselben Angaben, aus denen
`scan_usb_serial_ports()` und `map_modem_ports()` ihre Zuordnung bauen, und
seine Ausgabe wird zur Testfixture.

## Erwartete Stolperstellen

**`new_id` bindet alles.** Es transportiert kein `driver_info`, also gibt es
keine Interface-Reservierung, und `option` nimmt auch die Netzwerk-Interfaces.
Fuer AP-M2 ist das harmlos -- es wird kein Netzwerkpfad aufgebaut -- und sogar
nuetzlich, weil dann sichtbar wird, wieviele ttyUSB entstehen. Genau diese Zahl
beantwortet UNKNOWN 1 aus `ec200a-reuse-map.md`: erscheinen fuenf ttyUSB statt
drei, hat die Netzwerkfunktion zwei Interfaces und der spaetere `option.c`-
Eintrag braucht `.reserved = BIT(0) | BIT(1)`.

**Das Modem steht vermutlich auf RNDIS.** Im Referenzprojekt wurde es zuletzt
mit `AT+QCFG="usbnet",3` auf RNDIS gestellt. Das aendert an den seriellen Ports
nichts und ist fuer AP-M2 ohne Belang -- in AP-M2 wird die Einstellung
ausdruecklich NICHT angefasst, weil `AT+QCFG` persistent ist und `AT+CFUN=1,1`
eine Re-Enumeration ausloest.

**Der AT-Port ist nicht der erste ttyUSB.** Deshalb existiert die ganze
Zuordnung. `/dev/ttyUSB2` fest zu verdrahten ist der Fehler, der erst beim
Kunden auffaellt.

## Nachweis

AP-M2 ist erst dann erfuellt, wenn hier steht:

    AT   -> OK
    ATI  -> Quectel / EC200A / <Firmwarekennung>

gelesen ueber den Port, den `map_modem_ports()` als `at` ausgibt -- nicht ueber
einen, den jemand durchprobiert hat.

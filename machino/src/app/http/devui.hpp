// Die Geraeteseite, von machino selbst unter /machino/devices ausgeliefert.
//
// Dieselben Regeln wie bei netui.hpp, und aus denselben Gruenden:
//
//   * Der Installer patcht p/header.cgi NICHT von sich aus. Der Menueeintrag
//     ist ein eigener, ausdruecklicher Schritt (install.sh --with-device-page).
//     Die Seite ist ohne ihn per URL erreichbar.
//   * Einkompiliert statt als Datei installiert: Seite und API koennen so
//     nicht in verschiedenen Versionen auseinanderlaufen, und ein Binary-
//     Upgrade kann kein altes Asset zuruecklassen.
//   * Kein Framework, kein CDN, keine Schriftart von aussen. Eine Kamera am
//     eigenen Access Point hat kein Internet, und genau dann braucht man diese
//     Seite.
//
// Was sie NICHT ist: ein Ersatz fuer OpenIPCs Netzwerkseite. Sie richtet die
// Hardwareunterstuetzung ein -- Treiber nach /lib/modules, Profil nach
// /etc/wireless/usb -- damit die UNVERAENDERTE OpenIPC-Seite den Adapter
// danach im Dropdown "Wireless Adapter" anbietet. Bedient wird das WLAN
// weiterhin dort. Diese Trennung ist der Kern des Befunds vom 2026-09-25: der
// Treiber lief, aber niemand hatte ihn dem Wirtssystem gemeldet.
#pragma once
#include <cstddef>

namespace machino { namespace http {

// NUL-terminiertes HTML. Statischer Speicher; wird nie freigegeben, nie
// veraendert.
const char* machino_devices_page();
size_t      machino_devices_page_len();

}} // namespace machino::http

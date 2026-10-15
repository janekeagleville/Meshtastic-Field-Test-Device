# Meshtastic Mazowsze — CYD Field Tester

Firmware dla CYD ESP32-2432S028 połączonego przez UART z Heltec Wireless Tracker.
Tracker zachowuje standardowy Meshtastic oraz GPS; CYD jest klientem API i ekranem pomiarowym.

## Funkcje

- Ekran startowy MESHTASTIC MAZOWSZE FIELD TESTER (include/splash.h, RGB565 320x240) i spójny ciemnozielony interfejs: nagłówek, pasek GPS, panele, wskaźnik RSSI, kropki przekazań, wykres.
- Znacznik czytelności odbioru: CZYTELNY / GRANICZNY / NIECZYTELNY (progi w include/config.h).
- Lista do 64 nadawców z pomiarami odebranymi podczas bieżącej sesji, najnowsze na górze.
- RSSI (dBm), SNR (dB), czas od odbioru; dotknięcie wiersza otwiera szczegóły.
- W szczegółach: licznik, średnia/minimum/maksimum RSSI i wykres 48 ostatnich próbek.
- Droga pakietu: bezpośrednia, przekazana lub nieustalona. Zerowy/nieobecny hop_start jest celowo traktowany jako nieustalony.
- Ostatnia znana pozycja lokalnego Trackera, niezależnie od pozycji odległych węzłów.
- Zapis na microSD: przycisk ZAPIS na ekranie pracy tworzy plik `Nazwa-noda_DDMMYY_HH-MM-SS.csv`.
- Ponowienie połączenia API, heartbeat i kalibracja dotyku przechowywana w pamięci CYD.

## Ekrany

- Lista (NODE, RSSI, SNR, WIEK): kolorowa kropka i kolor RSSI według czytelności; numer strony po prawej.
- Szczegóły węzła: wskaźnik łukowy RSSI (-130...-30 dBm) ze znacznikiem czytelności, SNR z paskiem,
  skala „Przekazany" 0...7 hop (kropki), statystyki N/MIN/MAX/SR. i wykres ostatnich próbek z linią progu CZYTELNY.
- Zmiana skal i progów: GAUGE_MIN/MAX, SNR_MIN/MAX, HOP_MAX, SNR_GOOD/FAIR, RSSI_GOOD/FAIR w include/config.h.

Ocena dotyczy ostatniego odcinka radiowego: CZYTELNY gdy SNR >= -5 dB i RSSI >= -115 dBm,
GRANICZNY gdy SNR >= -15 dB i RSSI >= -125 dBm, w pozostałych przypadkach NIECZYTELNY.
Progi są założeniem startowym; zależą od presetu LoRa (np. LongFast dekoduje do ok. -17 dB SNR) — dostosuj je w config.h.
Brak pola SNR w proto3 jest odczytywany jako 0, co daje ocenę CZYTELNY; interpretuj ostrożnie.

## Połączenie

| Heltec Wireless Tracker | CYD |
|---|---|
| GPIO6, TX UART | IO35 na P3, RX |
| GPIO7, RX UART | IO27 na CN1, TX |
| GND | GND |

UART 3,3 V. Piny wybieraj po oznaczeniach na PCB, nie numerach pozycji w złączu.
GPIO6 i GPIO7 wybrano na podstawie dokumentacji Wireless Tracker V1.1 i V2 (ESP32-S3), z zachowaniem GNSS.
Zasilaj każdą płytkę przez jej USB; wspólny powerbank z dwoma wyjściami jest możliwy.
Nie łącz wyjść 3,3 V lub 5 V obu płytek. Tracker musi mieć właściwe anteny LoRa i GNSS.

W Meshtastic na Trackerze: Serial Enabled = true, Mode = PROTO, TXD = 6, RXD = 7,
Baud = 115200. GPS pozostaje włączony. Region, preset i kanały ustaw odpowiednio do swojej sieci.
To samo z CLI (`pip install meshtastic`, Tracker na USB):
`meshtastic --set serial.enabled true --set serial.mode PROTO --set serial.rxd 7 --set serial.txd 6 --set serial.baud BAUD_115200`
Firmware CYD nie ustawia kanałów ani parametrów radiowych i nie wysyła wiadomości LoRa.
Wysyła tylko lokalne żądanie konfiguracji API i heartbeat.

## Budowanie i wgrywanie

Projekt jest przeznaczony dla PlatformIO. Otwórz ten folder w VS Code z rozszerzeniem PlatformIO,
wybierz środowisko `cyd_st7789`, następnie Build i Upload, gdy CYD jest podłączony do USB.
PlatformIO pobierze platformę ESP32 i bibliotekę TFT_eSPI w wersjach wskazanych w platformio.ini.
Nie trzeba ręcznie edytować globalnego User_Setup.h biblioteki.

Z terminala z zainstalowanym PlatformIO:

```sh
pio run -e cyd_st7789
pio run -e cyd_st7789 -t upload
pio device monitor -b 115200
```

Domyślne środowisko to `cyd_st7789_noinv` (ST7789 bez inwersji kolorów). Jeśli czerń wychodzi jako biel,
a cyjan jako czerwień, użyj środowiska z odwrotną inwersją; gdy kolory są poprawne, ale wersja `cyd_st7789`
(z TFT_INVERSION_ON) daje właściwy obraz na Twoim egzemplarzu, wybierz ją.
Dla CYD z dwoma portami USB przyjęto ST7789, ale sam wygląd PCB nie potwierdza kontrolera.
Jeśli obraz nie działa lub jest uszkodzony, skompiluj alternatywne środowisko `cyd_ili9341`.
Kolory, orientacja i dotyk wymagają sprawdzenia na fizycznym panelu.

## Obsługa

Przy pierwszym uruchomieniu po logo dotknij kolejno czterech krzyżyków, aby skalibrować dotyk.
Kalibracja pozostaje zapisana po wyłączeniu.

- DALEJ: następna strona listy.
- Dotknięcie węzła: ekran szczegółów i wykres RSSI.
- LISTA: powrót do listy.
- ZAPIS (dół ekranu pracy, środkowy klawisz): tworzy nowy plik i zapisuje pakiety wybranego noda; podczas zapisu przycisk pokazuje STOP i liczbę wierszy. Z listy węzłów zapisuje wszystkie.
- STOP i licznik: zakończenie zapisu i zamknięcie pliku.
- KAL: powtórzenie kalibracji.
- BRAK SD: karta nie została zamontowana; dotknięcie ponawia próbę.
- BLAD SD: zapis lub otwarcie pliku nie powiodły się; stan jest też raportowany przez USB.
- Krótki przycisk BOOT: następna strona listy / wyjście ze szczegółów.
- Długi BOOT (>1 s): start/stop zapisu.

Monitor USB przy 115200: `l` start/stop zapisu, `t<epoch>` ręczne ustawienie zegara (np. `t1791650820`), `c` kalibracja, `r` ponowne połączenie API.

Kartę microSD sformatuj jako FAT32 i włóż przed uruchomieniem. Zatrzymaj zapis przed jej wyjęciem.
Dane są opróżniane z bufora co 2 s; nagłe odłączenie zasilania może utracić ostatnie rekordy lub uszkodzić plik.
SPI ekranu, SD i dotyku są rozdzielone: TFT HSPI (12/13/14/15), SD VSPI (19/23/18/5),
a XPT2046 obsługiwany programowo (MISO39/MOSI32/CLK25/CS33).

## Nazwy plików i czas
Nazwa: `<nazwa noda>_DDMMYY_HH-MM-SS.csv`, czas lokalny (Polska, CET/CEST). System FAT nie dopuszcza dwukropka w nazwie, więc godzina ma myślniki.
Płytka nie ma zegara: czas pochodzi z pakietów odebranych przez Tracker (wymaga ważnego czasu, np. po złapaniu GPS). Bez czasu plik nazywa się `Nazwa_BRAK-CZASU_001.csv`.
Karta musi być FAT32. Gdy firmware SD nie obsługuje długich nazw, powstaje `/LOGnnnnn.csv` (komunikat KROTKA NAZWA). Komunikat BRAK SD: sprawdź kartę i ponów ZAPIS.

## Format CSV

Separator: przecinek; separator dziesiętny: kropka. Importuj w Excelu jako CSV z tymi ustawieniami.
Jeżeli potrzebne jest rozszerzenie .csd, plik można przemianować; jego zawartość pozostaje CSV.

| Kolumna | Znaczenie |
|---|---|
| uptime_ms | Czas odbioru przez CYD od uruchomienia; licznik zawija się po ok. 49,7 dnia |
| rx_epoch | Czas odbioru z Trackera w sekundach Unix; 0 oznacza brak czasu |
| sender_id | Pełne ID pierwotnego nadawcy, szesnastkowo |
| packet_id | ID pakietu, szesnastkowo |
| rssi_dbm | RSSI ostatniego odcinka radiowego |
| snr_db | SNR ostatniego odcinka; brak pola w proto3 jest interpretowany jako 0 |
| hops | Liczba przeskoków lub -1 gdy nieustalona |
| relay_low_byte | Tylko ostatni bajt ID przekaźnika; nie identyfikuje go jednoznacznie |
| via_mqtt | Flaga MQTT; pakiety z tą flagą są wykluczane z pomiarów |
| transport | Mechanizm odbioru z protokołu; 0 także dla starszych wersji bez tego pola |
| tracker_lat, tracker_lon | Ostatnia znana pozycja lokalnego Trackera, w stopniach; puste przy braku pozycji |
| position_receipt_age_s | Czas od otrzymania tej pozycji przez CYD, nie od ustalenia GPS |
| position_live | 0 dla początkowej pozycji z bazy Trackera, 1 dla późniejszej aktualizacji API; nie potwierdza aktualnego fix GPS |
| position_source | 0 nieustalona, 1 ręczna, 2 wewnętrzny GPS, 3 zewnętrzny GPS |
| position_epoch | Czas przypisany pozycji (timestamp lub time z API), 0 gdy nieznany |
| gps_age_s | rx_epoch - position_epoch, jeśli oba czasy są dostępne i porównywalne; inaczej puste |

Współrzędne nie są pobierane z wiadomości odległego nadawcy. Przed wyjściem w teren sprawdź,
że Tracker ma aktualną pozycję z GPS (position_source=2), a gps_age_s jest rozsądne.
Gdy GPS przestanie dostarczać aktualizacji, zachowywana jest ostatnia pozycja i rośnie jej wiek.
Napis CACHE oznacza pozycję pobraną podczas początkowego odczytu bazy, o niepewnej aktualności.

## Granice pomiarów

RSSI/SNR po retransmisji opisują przekaźnik odbierany przez Tracker, a nie pierwotnego nadawcę.
Lista jest grupowana według pierwotnego nadawcy, dlatego używaj oznaczenia drogi i pola relay_low_byte.
API udostępnia pakiety przekazane do klienta; nie jest to surowy sniffer wszystkich transmisji ani pomiar
ciągły. Firmware nie oblicza utraty pakietów bez zewnętrznego, numerowanego źródła testowego.
Pakiety bez RSSI, pakiety lokalne i pakiety pochodzące z MQTT/UDP/API są pomijane.
Brak pomiaru nie jest zastępowany fikcyjnym RSSI z bazy NodeInfo.
Historia i statystyki są resetowane przy ponownym uruchomieniu CYD. Zapis nie startuje automatycznie.

## Stan weryfikacji

Sprawdzono parser protokołu w natywnym C++ z AddressSanitizer i UndefinedBehaviorSanitizer:
ujemny RSSI, zero RSSI vs brak pola, ujemne i zerowe współrzędne, uszkodzone dane,
ramkowanie i synchronizacja, nieznane pola oraz 10 000 losowych wejść.
Sprawdzono składnię aplikacji z uproszczonymi interfejsami bibliotek — to nie zastępuje kompilacji ESP32.
W tym środowisku nie było narzędzi PlatformIO/Arduino ani dostępu do pobrania zależności.
Warstwa ekranu (splash.h, ekrany, znacznik czytelności) przeszła sprawdzenie składni z uproszczonymi interfejsami bibliotek.
Nie wykonano kompilacji docelowej ESP32 ani testu fizycznego CYD/Trackera; pliku .bin nie dostarczono.
Stara wersja interfejsu: main.cpp.bak (jeden plik .cpp zastępujący src/main.cpp).

Test parsera, jeśli dostępny jest kompilator C++:

```sh
c++ -std=c++11 -I include test/protocol_test.cpp -o protocol-test
./protocol-test
```

## Dokumentacja źródłowa

- https://meshtastic.org/docs/configuration/module/serial/
- https://meshtastic.org/docs/development/device/client-api/
- https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto
- https://resource.heltec.cn/download/Wireless_Tracker/Wireless_Tracker%20_1.1.pdf
- https://resource.heltec.cn/download/Wireless_Tracker_V2/Wireless_Tracker_v2_Datasheet/Wireless%20Tracker%20v2.pdf
- https://github.com/Bodmer/TFT_eSPI

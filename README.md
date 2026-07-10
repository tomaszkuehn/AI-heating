# Kontroler ogrzewania na ESP32

Firmware sterownika ogrzewania dla ESP32-WROOM, realizujący autonomiczne
sterowanie urządzeniem grzewczym na podstawie temperatur z 1–6 czujników
wewnętrznych (i opcjonalnego czujnika zewnętrznego), z interfejsem WWW,
rejestracją danych, detekcją awarii i trybem symulacyjnym.

## Budowanie i wgranie

Wymaga ESP-IDF v5.x (testowane na v5.3.2, z toolchainem i Pythonem).

Zależność LittleFS (`joltwallet/littlefs`) jest zadeklarowana w
`main/idf_component.yml` i pobierana automatycznie przy pierwszym budowaniu —
nie trzeba dodawać jej ręcznie.

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Po pierwszym uruchomieniu urządzenie startuje jako AP `ESP` / `12345678`
(hasło WPA2 musi mieć min. 8 znaków; gdyby zapisana konfiguracja miała krótsze,
`repair_config()` przywraca domyślne, aby AP zawsze startował zaszyfrowany).
Panel WWW: `http://192.168.4.1/`.

## Architektura (spec pkt 13)

Firmware jest podzielony na moduły w `main/`:

| Plik                  | Moduł (spec)         | Odpowiedzialność                                                  |
|-----------------------|----------------------|-------------------------------------------------------------------|
| `sensor_manager.c`    | sensor_manager       | UART z zewn. interfejsem, średnia krocząca 7, temp. efektywna, walidacja, otwarte okno |
| `simulation_manager.c`| simulation_manager   | symulacja czujników i model cieplny budynku                       |
| `control_engine.c`    | control_engine       | automat stanów, histereza, profil dobowy, BOOST, wybieg, awaryjny |
| `heating_output.c`    | heating_output       | linia GPIO, impulsy wybiegu pompy, symulacja wyjścia              |
| `fault_manager.c`     | fault_manager        | detekcja awarii z modelem odpowiedzi cieplnej, powiadomienia       |
| `storage_manager.c`   | storage_manager      | NVS (konfig) + LittleFS (profile, historia, logi), agregacja      |
| `network_manager.c`   | network_manager      | Wi-Fi AP/STA, provisioning, SNTP, przycisk resetu sieci           |
| `web_ui_api.c`        | web_ui_api           | serwer HTTP + REST API + SPA (`web/`)                             |
| `notification_manager.c` | notification_manager | e-mail (SMTP) / SMS (brama HTTP), wysyłane z osobnego zadania workera (kolejka FreeRTOS, off-loop)                              |
| `profile.c`           | —                    | profil dobowy 24h: walidacja, JSON, zapis/odczyt                  |
| `data_model.h`        | —                    | wspólne typy (stan, jakość, czujnik, profil)                      |

Warstwa sterowania (`control_task`) działa w osobnym zadaniu FreeRTOS i nie
zależy od warstwy WWW — awaria serwera HTTP nie blokuje sterowania (spec 13).

## Pamięć trwała (spec pkt 9–10)

- **NVS** — konfiguracja klucz-wartość (sieć, czujniki, wagi, offsety,
  alarmy, tryby, flagi) oraz liczniki zużycia flash.
- **Samonaprawa konfiguracji po aktualizacji** — odczyt konfiguracji jest
  **tolerancyjny na rozmiar** (`storage_load_config`): gdy zapisany blob jest
  krótszy niż aktualny `system_config_t` (np. po aktualizacji firmware z nowymi
  polami w środku struktury), nieodczytany ogon jest zerowany, a `repair_config`
  przywraca domyślne dla pustej nazwy urządzenia i nieprawidłowych limitów
  (`fault_grace_sec` / `max_on_sec` / `max_on_break_sec` < 60 s). Sprzęt
  samonaprawia się po aktualizacji — bez `erase_flash` (WiFi i reszta konfigu
  przetrwają). `fault_manager` ma dodatkowo zero-fallback karencji, więc nawet
  „wyzerowany" limit nie wywoła fałszywej awarii `NO_HEAT_RISE`.
- **LittleFS** — pliki użytkownika, profile dobowe, agregaty dobowe i logi
  (odporny na zaniki zasilania, lepszy od SPIFFS do logowania).
- **Historia minutowa (24 h) jest wyłącznie w RAM** — bufor pierścieniowy
  `s_ring[HE_RING_SIZE = 1440]` (`storage_manager.c`). Wykres 24 h czyta ten
  bufor bezpośrednio (`storage_ring_copy()`), więc zawsze pokazuje najświeższe
  dane, przewija się w lewo i **nie zużywa flasha**. Skutek uboczny: po zaniku
  zasilania wykres 24 h startuje pusty (dane ulotne), ale konfiguracja i
  agregaty dobowe przetrwają.

### Ochrona flash przed szybkim zużyciem

Zasada: pisać do flasha **rzadko, partiami i bez nieograniczonego przyrostu
plików**. Mechanizmy (w `storage_manager.c`):

1. **Historia minutowa tylko w RAM.** Próbki minutowe nie trafiają na flash —
   `storage_record_minute()` zapisuje je do bufora pierścieniowego, a
   `storage_flush_samples()` jest pustym no-op. Zamiast ~1440 zapisów/dobę
   powstaje **1 zapis agregatu na dobę**.
2. **Agregacja dobowa + kasowanie surowych danych.** Przy zmianie dnia próbki
   z bufora RAM są redukowane do jednego wiersza (~20 B) w `daily.csv`
   (`aggregate_day()`), a ewentualne stare pliki `YYYYMMDD.csv` z poprzednich
   wersji firmware są usuwane (`prune_old_samples()`). Agregacja odbywa się
   **tylko gdy zegar jest zsynchronizowany SNTP** (`he_time_valid()` — flaga
   `s_cur_day_real`): zanim urządzenie pobierze czas z sieci, wirtualny zegar
   jest zasiewany na epokę `HE_TIME_VALID_EPOCH` (2023-11-14), więc próbki
   z tego czasu **nie są agregowane** (nie powstają sztuczne wiersze „20231114").
   Dodatkowo przy starcie `dedup_daily_csv()` czyści ewentualne powielone /
   nieposortowane wiersze z poprzednich uruchomień (zachowuje **ostatni** wiersz
   na każdy dzień, sortuje rosnąco) — `daily.csv` jest zawsze krótki i
   posortowany.
3. **Kompaktowanie logu zdarzeń.** `events.log` ma twardy limit
   `HE_LOG_MAX_BYTES = 16 KB`; po przekroczeniu zachowywana jest tylko nowsza
   połowa, wyrównana do pełnych wierszy (zapis do `.tmp` + `rename`,
   `compact_log_tail()`). Ponadto `raise_fault()` **limituje częstotliwość**
   zapisu: ten sam typ awarii (`fault_class_t`) trafia do logu co **najwyżej
   raz na 60 s** (`s_last_log_fault` / `s_last_log_us`), więc oscylacje awarii
   (np. `NO_HEAT_RISE` ↔ `SENSOR_IFACE`) nie zalewają logu tysiącami wpisów
   między kompakcjami. Log można też **wyczyścić ręcznie** przyciskiem „Wyczyść
   log" w sekcji „Logi / alarmy" (`POST /api/log/clear` → `storage_clear_log()`)
   — usuwa cały plik `events.log` (operacji nie da się cofnąć).
4. **Konfiguracja w NVS zapisywana tylko przy zmianie.** `storage_save_config()`
   jest wołane przy faktycznej edycji z UI/API, nie cyklicznie; NVS ma własny
   wear-leveling.
5. **Wybór LittleFS zamiast SPIFFS.** Copy-on-write i wbudowany wear-leveling
   są lepsze do zapisów logopodobnych i odporne na zanik zasilania.

### Wskaźnik i analiza zużycia flash

`storage_get_flash_wear()` liczy szacunkowe zużycie na podstawie trwałych
liczników w NVS (`nvs_writes` — commity konfiguracji, `fs_writes` —
skumulowane KB zapisane do LittleFS). Parametry pamięci ESP32 (NOR, np.
W25Q32): sektor **4 KB**, wytrzymałość **100 000 cykli kasowania/sektor**,
założona amplifikacja zapisu LittleFS **~2×**. Szacunek:
`cykle ≈ zapisane_KB / 4 KB × 2`, procent = `cykle / 100 000 × 100`.
Wartości (`flash_wear_pct`, `flash_erase_cycles`, `flash_nvs_writes`) są w
`/api/state` i `/api/diagnostics`, a na pulpicie kafelek „Pamięć flash (dane)"
pokazuje użycie KB i procent zużycia (żółty od 0,5%, czerwony od 1%).

## Interfejs WWW (`web/`)

Jednostronicowa aplikacja (bez zależności) serwowana z firmware. Zasoby WWW
(`index.html`, `style.css`, `app.js`) są **kompresowane gzip w trakcie
budowania** (`main/CMakeLists.txt`, `gzip -9 -n -f`) i serwowane z nagłówkiem
`Content-Encoding: gzip` — przeglądarka dekompresuje je transparentnie. To
odchudza firmware o ~35 KB (z ~816 B do ~36 KB wolnego w partycji 1 MB) bez utraty
funkcji. Flaga `-f` nadpisuje istniejący `.gz` z poprzedniej kompilacji — bez niej
przyrostowa edycja pliku WWW kończy się błędem `X.gz already exists; not
overwritten` (patrz komentarz w `main/CMakeLists.txt`). Możliwe sekcje:

- **Wykres temperatur 24 h** — oś X w czasie rzeczywistym (okno kotwiczone do
  najnowszej próbki, więc przewija się w lewo w miarę napływu danych), ze skalą
  godzinową. Rysuje temperaturę systemową, zewnętrzną oraz osobną linię dla
  każdego aktywnego czujnika wewnętrznego. W tło nałożony jest profil dobowy
  (pasmo ON/OFF). **Czerwony pasek na dole** pokazuje minuty, w których
  przekaźnik grzania był aktywny.
  **Czujniki z awarią** (TIMEOUT, STALE, OUT_OF_RANGE) **nie są rysowane**
  na wykresie — ich linia znika do czasu powrotu do stanu OK, a w legendzie
  pojawia się znacznik ❌. Zapobiega to zanieczyszczaniu wykresu zamrożonymi
  lub błędnymi odczytami.
- **Zoom osi czasu** — przyciski **1h · 6h · 12h · 24h** nad wykresem.
  Tiki osi X dostosowują się automatycznie: co 15 min dla okna 1 h, co 1 h
  dla 6 h, co 3 h dla szerszych. Aktywny przycisk jest podświetlony.
- **Wybór czujników na wykresie** — kolorowe checkboxy z nazwą czujnika
  (w jego kolorze linii) pozwalają pokazać/ukryć poszczególne czujniki
  wewnętrzne bez przeładowania strony. Domyślnie wszystkie widoczne.
- **Health check czujników** — w tabeli czujników kolumna **Health** z kolorową
  kropką: 🟢 zielona = OK/SIMULATED, 🟡 żółta = WINDOW_OPEN, 🔴 czerwona =
  TIMEOUT/STALE/OUT_OF_RANGE, ⚫ szara = DISABLED. **Kliknięcie** na czerwoną
  lub żółtą kropkę rozwija panel ze szczegółowym opisem problemu: przyczyna,
  czas od ostatniego odczytu (`last_seen`) i skutek dla systemu (np.
  wykluczenie ze średniej). Dane o wieku odczytu pochodzą z pola `last_seen`
  (sekundy od ostatniej aktualizacji), dodanego do `/api/state`.
- **Wykres zużycia energii (12 mies.)** — słupki minut grzania na dobę
  (pomarańczowe) z nałożoną linią średniej temperatury systemowej (niebieska).
  Dane z `daily.csv` (kolumna `heat_mins`).
- Sekcje konfiguracyjne (profil dobowy, wybieg pompy / tryb awaryjny,
  **zabezpieczenia i limity**, sieć, powiadomienia, symulacja) są domyślnie
  zwinięte do paska nagłówka i rozwijane kliknięciem.
- **Sieć / Powiadomienia pokazują bieżące ustawienia** — SSID i tryb (klient/AP)
  oraz odbiorcę/serwer SMTP/użytkownika/telefon/bramę SMS. Hasła (Wi-Fi, SMTP)
  nie są pokazywane; puste pole hasła przy zapisie zachowuje bieżące (patrz
  sekcja E).
- **Profil dobowy** — grid **12 kolumn × 2 rzędy** (12 godzin w wierszu).
  Temperatura wyłączenia (OFF) nad załączenia (ON). Trzy sloty pamięci
  **na urządzeniu** (1/2/3) z jedno-klikowym zapisem i odczytem
  (`/api/profile/file?name=profile_N`). **Eksport do pliku .json** (pobranie
  na komputer) i **import z pliku** (wgranie z powrotem). Przycisk
  „Zastosuj" waliduje i zapisuje do aktywnej konfiguracji.
- **Responsywność** — na urządzeniach mobilnych tabela czujników przewija się
  poziomo (nie wychodzi poza kartę), siatka kafelków dashboardu zwija się do
  2 kolumn, a wykresy dopasowują szerokość do ekranu.
- **Pulpit** — nagłówek panelu zawiera **klikalną nazwę urządzenia**
  (edycja inline, zapis w NVS), **zegar systemowy** (HH:MM:SS
  z czasu SNTP lub wirtualnego), wskaźnik zdrowia systemu, stan automatu,
  adres IP urządzenia i status synchronizacji czasu (🕐 zielony = SNTP,
  żółty = lokalny). Kafelki pokazują: temperatury, liczbę sprawnych
  czujników, uptime, zużycie flash, czas grzania w oknie (24h/zoom) i status
  pieca.
- **Powiadomienie po restarcie** — **60 sekund po uruchomieniu** (liczone po
  rzeczywistym uptime `esp_timer_get_time()`, więc działa poprawnie także w
  trybie symulacji/przyspieszenia ×10) wysyłane jest jednorazowe powiadomienie.
  Wysyłka odbywa się **z osobnego zadania workera** (kolejka FreeRTOS,
  off-loop) — blokujące I/O SMTP/SMS nie blokuje pętli sterowania i nie
  wyzwala task-watchdog; worker dodatkowo **ponawia do 30 s**, gdy Wi-Fi nie
  zdąży jeszcze powstać. E-mail zawiera szczegółowy raport: nazwa urządzenia,
  temperatury systemowa i zewnętrzna, liczba sprawnych czujników, stan każdego
  czujnika wewnętrznego (nazwa, jakość, temperatura efektywna) **oraz czujnika
  zewnętrznego**, gdy jest skonfigurowany. SMS to krótka wiadomość
  `[nazwa] RESTART`. Wysyłka tylko gdy skonfigurowany e-mail (`email_enabled`)
  i/lub SMS (`sms_enabled`). Konfiguracja w karcie „Powiadomienia".
- **Nazwa urządzenia** konfigurowalna przez kliknięcie tytułu w nagłówku
  dashboardu lub przez `POST /api/device {"name":"..."}`. Domyślnie
  „Sterownik CO". Przetrzymuje restart (NVS). Używana w temacie i treści
  powiadomień restartowych. **Walidacja:** znaki `"`, `\` i kontrolne (<0x20)
  są usuwane przy zapisie, a pusta/w całości odrzucona nazwa → HTTP 400 (zapobiega
  zepsuciu JSON `/api/state` i wstrzyknięciu nagłówków SMTP); UTF-8 (np. polskie
  znaki) jest dozwolone, a w temacie powiadomień kodowane RFC 2047
  (`=?UTF-8?B?…?=`), gdy zawiera znaki non-ASCII.
- **Status pieca** — kafelek z dużym kołem:
  - 🔥 **czerwone koło + płomień** = grzanie aktywne, podpis „Grzeje"
  - 🔵 **niebieskie koło** = grzanie włączone, ale nie grzeje, podpis „Nie grzeje"
  - ◯ **szare koło + ✕** = ogrzewanie wyłączone (kill switch), podpis „Wyłączony"

### Dostęp przez mDNS

W trybie STA (klient routera) urządzenie rejestruje nazwę **`heating.local`**
przez mDNS (`espressif/mdns`). Panel jest wtedy dostępny jako
**`http://heating.local/`** — nie trzeba znać adresu IP przydzielonego przez
DHCP. Wymaga obsługi mDNS/Bonjour po stronie klienta (Windows 10+ natywnie,
Linux wymaga `avahi-daemon`, Android/iOS natywnie).

Adres IP urządzenia jest też widoczny w nagłówku panelu „Pulpit" (obok stanu systemu).

## Konfigurowalne limity i zabezpieczenia

Karta **„Zabezpieczenia i limity"** w UI (domyślnie zwinięta) oraz endpoint
`POST /api/limits` umożliwiają zmianę trzech parametrów ochronnych bez
przekompilowywania firmware. Wszystkie są przechowywane w NVS jako część
`system_config_t` i przetrwają restart.

| Parametr | Pole JSON | Domyślnie | Zakres | Opis |
|---|---|---|---|---|
| Karencja awarii grzania | `fault_grace_sec` | 300 s (5 min) | 60–3600 s | Czas od startu grzania, po którym `fault_manager` zaczyna oceniać skuteczność — daje instalacji czas na reakcję cieplną |
| Maks. ciągłe grzanie | `max_on_sec` | 14400 s (4 h) | 300–86400 s | Po przekroczeniu tego czasu przekaźnik jest **twardo wyłączany** (niezależnie od histerezy) — ochrona przed „zawieszonym" termostatem |
| Przerwa po maks. grzaniu | `max_on_break_sec` | 600 s (10 min) | 60–86400 s | Wymuszony postój po wyłączeniu przez `max_on_sec` — sterownik ignoruje temperaturę i nie załączy grzania, dopóki przerwa nie minie |

**Mechanizm:** w `control_engine.c` zmienna `s_max_on_break_ms` odlicza pozostały
czas przerwy (wraz z blokadą antyoscylacyjną na początku `control_tick()`).
W `hysteresis_decision()` warunek `s_max_on_break_ms > 0` wymusza `false`
niezależnie od progu `on_temp` — przerwa jest absolutna. Gdy `s_on_ms` przekroczy
`max_on_sec`, ustawiane jest `s_max_on_break_ms = max_on_break_sec * 1000`.

**Karencja awarii:** parametr zastąpił stałą `s_grace_ms = 300000` w
`fault_manager.c` — `fault_manager_observe()` pobiera go z `s_cfg->fault_grace_sec`
i używa do oceny `NO_HEAT_RISE` oraz `LOW_HEAT_RISE`.

## Zdarzenia obsługiwane przez system

Dla każdego zdarzenia opisano: **warunek wystąpienia**, **sposób zakończenia**
oraz **akcje i rezultat** w działaniu systemu. Wszystkie awarie i przejścia
stanów trafiają do trwałego dziennika `events.log` (widoczny w UI: „Logi /
alarmy"), a awarie dodatkowo wyzwalają powiadomienia (patrz niżej).

Wpisy sprzed synchronizacji SNTP — gdy `time(NULL)` zwraca jeszcze czas
uruchomienia, a nie czas rzeczywisty — są wyświetlane jako **`boot +Ns`**
(liczba sekund od startu), a nie jako data z 1970 r.; dzięki temu pierwszy
wpis po restarcie (np. `FAULT_RESTART`) ma czytelny timestamp. Sekcja „Logi /
alarmy" ma przycisk **„Wyczyść log"** (`POST /api/log/clear` →
`storage_clear_log()`), który usuwa cały `events.log` — operacji nie da się
cofnąć.

### A. Zdarzenia jakości i awarii czujników (`sensor_manager.c`)

Każdy czujnik ma status `sensor_quality_t`, wyznaczany w każdym cyklu odpytania
(`sensor_manager_poll`, co `HE_CONTROL_TICK_MS = 1000 ms`). Status decyduje, czy
odczyt czujnika **wchodzi do średniej systemowej** — `sensor_manager_system_temp()`
uwzględnia wyłącznie czujniki o statusie `OK` lub `SIMULATED`.

#### `QUAL_TIMEOUT` — brak komunikacji z czujnikiem
- **Warunek:** brak świeżego odczytu przez `HE_SENSOR_TIMEOUT_SEC = 90 s`
  (`mono_ms() - last_update_ms > 90 s`), albo interfejs UART nie odpowiedział na
  poll (dla czujników rzeczywistych), albo źródło symulacji zwróciło NaN.
- **Zakończenie:** automatycznie, gdy nadejdzie poprawna ramka i czujnik znów
  otrzyma świeży odczyt (status wraca do `OK`).
- **Akcje i rezultat:** czujnik jest **wykluczony ze średniej systemowej**; jego
  waga jest pomijana, a pozostałe czujniki proporcjonalnie przejmują udział
  (średnia ważona po malejącej sumie wag). Jeśli w wyniku wszystkie czujniki
  wypadną — patrz `FAULT_SENSOR_IFACE` niżej. Na wykresie linia czujnika ma
  przerwę (wartość `-99`/NaN).

#### `QUAL_OUT_OF_RANGE` — odczyt poza zakresem logicznym
- **Warunek:** odczyt < `HE_TEMP_MIN_LOGICAL (-40 °C)` lub > `HE_TEMP_MAX_LOGICAL
  (85 °C)` — typowo zwarcie/rozwarcie toru pomiarowego lub błąd interfejsu.
- **Zakończenie:** automatycznie, gdy kolejny odczyt wróci do zakresu (status
  `OK`).
- **Akcje i rezultat:** odczyt **odrzucony**, czujnik wykluczony ze średniej.
  Chroni sterowanie przed reakcją na wartość absurdalną (np. −50 °C nie wywoła
  ciągłego grzania).

#### `QUAL_STALE` — odczyt „zamrożony" (podejrzenie uszkodzenia)
- **Warunek:** temperatura efektywna nie zmieniła się o >0,05 °C przez **ponad
  10 minut** (`stale_detect`) — realny czujnik prawie zawsze lekko dryfuje, więc
  idealna stałość sugeruje zawieszony/uszkodzony czujnik lub interfejs.
- **Zakończenie:** automatycznie przy pierwszej znaczącej zmianie odczytu
  (>0,05 °C) — licznik bezruchu jest zerowany, status wraca do `OK`.
- **Akcje i rezultat:** czujnik przechodzi ze `OK` w `STALE` i **wypada ze
  średniej systemowej**, dopóki nie zacznie znów reagować. Zapobiega
  „przyklejeniu" sterowania do martwej wartości.

#### `QUAL_WINDOW_OPEN` — wykrycie otwartego okna (spec 6.1)
- **Warunek:** lokalny szybki spadek — `drop > 1,5 °C` względem bazy **oraz**
  spadek o >1,0 °C większy niż średni spadek pozostałych aktywnych czujników
  (`window_detect`). Porównanie z innymi czujnikami odróżnia otwarte okno od
  naturalnego wychłodzenia całego budynku (unika fałszywych alarmów).
- **Zakończenie:** automatycznie, gdy temperatura odbuduje się do wnętrza
  histerezy bazy (`drop < HE_WINDOW_RECOVERY_HYST = 0,4 °C`), lub **ręcznie**
  przyciskiem „Przywróć" w tabeli czujników (`/api/sensor/restore`,
  `sensor_manager_restore`).
- **Akcje i rezultat:** czujnik jest **czasowo tłumiony** — wypada ze średniej
  systemowej, więc chwilowe wychłodzenie przy oknie **nie wymusza grzania**
  całego obiektu. Baza czujnika nie jest w tym czasie adaptowana. Po zamknięciu
  okna czujnik wraca do `OK`, a baza jest ustawiana na bieżącą temperaturę.

#### `QUAL_DISABLED` — czujnik wyłączony przez użytkownika
- **Warunek:** odznaczenie „Aktywny" w tabeli czujników (`active = false`,
  `/api/sensor`). Dotyczy zarówno czujników wewnętrznych, jak i **zewnętrznego**.
- **Zakończenie:** ponowne włączenie czujnika w UI.
- **Akcje i rezultat:** czujnik **nie jest odpytywany ani symulowany** i nie
  wpływa na nic. Wyłączenie **samoczynnie czyści flagę `window_open`**
  (`s->window_open = false` w `sensor_manager_poll`), więc po ponownym
  włączeniu czujnik nie dziedziczy przestarzałego stanu „otwarte okno" sprzed
  wyłączenia. Dla czujnika zewnętrznego oznacza to, że
  `sensor_manager_external_temp()` zwraca NaN — na pulpicie „Temp. zewn."
  pokazuje `--`, a model kompensacji cieplnej / symulacji używa wartości
  zastępczej (5 °C), a nie „ducha" wyłączonego czujnika.

#### `QUAL_SIMULATED` — odczyt ze źródła symulacji
- **Warunek:** czujnik ma ustawione źródło symulacji (≠ `SIM_SRC_REAL`) i jest
  aktywny. Traktowany w sterowaniu **równorzędnie z `OK`** (wchodzi do średniej).
- **Zakończenie:** przełączenie źródła z powrotem na „Rzeczywisty".
- **Akcje i rezultat:** pozwala testować logikę bez sprzętu; w UI zaznaczone
  kolorem/etykietą.

### B. Awarie systemowe (`fault_class_t`, `fault_manager.c`)

Awaria jest podnoszona przez `raise_fault()`: ustawia stan awarii, **loguje**
zdarzenie (severity 2) i **jednokrotnie** wysyła powiadomienie (flaga
`s_notified` blokuje spam do czasu skasowania). Logowanie jest dodatkowo
**limitowane czasowo**: ten sam typ awarii (`fault_class_t`) trafia do
`events.log` co najwyżej raz na 60 s (`s_last_log_fault` / `s_last_log_us`),
więc szybkie oscylacje (np. `NO_HEAT_RISE` ↔ `SENSOR_IFACE`) nie zalewają logu
tysiącami wpisów. Tylko jedna awaria jest aktywna naraz (o najwyższym
priorytecie wykrycia).

#### `FAULT_SENSOR_IFACE` — wszystkie czujniki niedostępne
- **Warunek:** `total_sensors > 0` i `healthy_sensors == 0` — żaden czujnik nie
  ma statusu `OK`/`SIMULATED` (np. cały interfejs UART padł, wszystkie w
  `TIMEOUT`).
- **Zakończenie:** **automatyczne** — gdy choć jeden czujnik wróci do zdrowia
  (`healthy_sensors > 0`), awaria jest kasowana samoczynnie (analogicznie do
  `FAULT_NETWORK`).
- **Akcje i rezultat:** domyślnie stan sterownika przechodzi w `ST_FAULT`,
  **przekaźnik wyłączony** (`target_relay = false`) — brak wiarygodnych danych =
  brak grzania ze zwykłej histerezy. Bezwarunkowy tryb awaryjny cykliczny
  (`emergency.enabled`) ma wyższy priorytet i podtrzymuje minimalne grzanie mimo
  braku czujników (działa też przy sprawnych czujnikach). Dodatkowo opcja
  **`emergency_on_sensor_fault`** (domyślnie **WYŁ**) zmienia zachowanie tylko
  dla tej awarii: zamiast `ST_FAULT`/OFF utrzymuje ten sam duty cycle
  (`on_seconds` co `period_seconds`) przez czas trwania `FAULT_SENSOR_IFACE` —
  ochrona przeciwzamrożeniowa na wypadek długiej awarii czujników. Jest to
  wariant **warunkowy** (tylko gdy awaria aktywna), w przeciwieństwie do
  bezwarunkowego `emergency.enabled`; oba współdzielą parametry `on`/`period` i
  akumulator fazy. Po odzyskaniu choć jednego czujnika awaria kasuje się
  samoczynnie i sterowanie wraca do histerezy.

#### `FAULT_NO_HEAT_RISE` — brak wzrostu temperatury przy grzaniu
- **Warunek:** grzanie aktywne dłużej niż karencja `fault_grace_sec` (domyślnie
  5 min, konfigurowalne w karcie „Zabezpieczenia i limity"), a
  zaobserwowane tempo wzrostu `observed ≤ 0` (temperatura nie rośnie mimo
  załączonego pieca). Model nie jest naiwny — porównuje z **wyuczonym tempem**
  `s_learned_rate` (EMA z udanych cykli).
- **Zakończenie:** automatycznie — gdy przy kolejnym grzaniu (po karencji)
  `observed ≥ 0,5 × s_learned_rate`, awaria jest kasowana.
- **Akcje i rezultat:** log + powiadomienie („no temperature rise while
  heating"). Sygnalizuje np. brak paliwa/zapłonu, zamknięty zawór, awarię pieca.
  Sterowanie nie jest twardo blokowane (awaria informuje operatora), ale stan
  zdrowia (`health_ok`) jest fałszywy — czerwony marker w UI.

#### `FAULT_LOW_HEAT_RISE` — wzrost poniżej oczekiwanego
- **Warunek:** jak wyżej, ale `0 < observed < 0,5 × expected`, gdzie `expected`
  to wyuczone tempo skorygowane o temperaturę zewnętrzną
  (`expected = s_learned_rate − 0,01 × (T_sys − T_zewn)`, min. 0,05 °C/min) —
  zimniej na zewnątrz = większe straty = niższy akceptowalny przyrost.
- **Zakończenie:** automatycznie, gdy przyrost wróci do ≥ 0,5 × wyuczone tempo.
- **Akcje i rezultat:** log + powiadomienie („heating rise below expected
  (airlock/valve?)") — typowo zapowietrzenie instalacji lub przymknięty zawór.
  Grzanie trwa, ale operator jest ostrzeżony.

#### `FAULT_NETWORK` — brak sieci Wi-Fi
- **Warunek:** `network_up == false` i brak innej aktywnej awarii (najniższy
  priorytet — informacyjna).
- **Zakończenie:** **automatyczne** — po ponownym połączeniu (`network_up`)
  `fault_manager` sam kasuje tę awarię.
- **Akcje i rezultat:** **nie blokuje sterowania** — piec pracuje dalej wg
  profilu. W maszynie stanów `FAULT_NETWORK` jest jawnie wykluczony z warunku
  wejścia w `ST_FAULT`. Służy tylko sygnalizacji i powiadomieniu (jeśli droga
  powiadomień działa mimo braku sieci lokalnej).

#### `FAULT_STORAGE` — błąd pamięci/systemu plików
- **Warunek:** `storage_ok == false` (`storage_healthy()` — np. nieudany zapis
  NVS lub niezamontowany LittleFS).
- **Zakończenie:** po skasowaniu awarii, gdy pamięć znów jest sprawna.
- **Akcje i rezultat:** log + powiadomienie. Sterowanie działa dalej (grzanie
  nie zależy od zapisu historii), ale rejestracja danych/konfiguracji jest
  niepewna — sygnalizowane w diagnostyce i markerze zdrowia.

#### `FAULT_RESTART` — nieoczekiwany restart
- **Warunek:** przy starcie `esp_reset_reason()` zwraca powód inny niż
  POWERON/SW/DEEPSLEEP (np. panic, watchdog, brownout) — wykrywane w `main.c`.
- **Zakończenie:** zdarzenie jednorazowe (log przy starcie); nie „trwa".
- **Akcje i rezultat:** wpis do `events.log` (severity 2) — ślad do diagnostyki
  niestabilności zasilania/oprogramowania. Konfiguracja jest odtwarzana z NVS,
  a uszkodzony profil naprawiany (`repair_config`).

> `FAULT_SENSOR` jest klasą zarezerwowaną dla awarii pojedynczego czujnika;
> obecnie utrata wszystkich czujników jest raportowana jako `FAULT_SENSOR_IFACE`.

### C. Zdarzenia sterowania i stany pieca (`control_engine.c`)

Automat stanów rozstrzyga priorytetowo (malejąco): kill switch → BOOST → tryb
awaryjny → awaria → histereza normalna. Każda **zmiana stanu** jest logowana
(`enter_state`, severity 0).

| Zdarzenie | Warunek | Zakończenie | Akcje / rezultat |
|---|---|---|---|
| **Grzanie ON (histereza)** | `T_sys ≤ on_temp` danej godziny profilu, minęło `HE_MIN_OFF_SEC = 120 s` OFF i brak blokady antyoscylacyjnej | `T_sys ≥ off_temp` po min. `HE_MIN_ON_SEC = 120 s` ON | przekaźnik ON, stan `ST_HEATING` |
| **Grzanie OFF (histereza)** | `T_sys ≥ off_temp`, min. czas ON dotrzymany, brak blokady | spadek `T_sys ≤ on_temp` | przekaźnik OFF, stan `ST_IDLE` (lub wybieg pompy) |
| **Blokada antyoscylacyjna** | każde wejście w nowy stan ustawia `HE_ANTIOSC_LOCK_SEC = 180 s` | odliczenie do zera (skalowane ×10 w symulacji) | wstrzymuje przełączenie ON↔OFF, tłumi migotanie na progu |
| **Zabezpieczenie MAX ON** | ciągłe grzanie ≥ `max_on_sec` (domyślnie 4 h, konfigurowalne) | po upływie `max_on_break_sec` (domyślnie 10 min, konfigurowalne) | wymusza OFF + twardą przerwę niezależnie od histerezy (ochrona przed „zawieszonym" termostatem) |
| **BOOST 5 min** | przycisk „Grzanie 5 min" (`/api/boost`) | upływ `HE_BOOST_DURATION_SEC = 300 s` lub ponowne kliknięcie (anuluj) | wymusza grzanie ponad histerezę; po zakończeniu wraca do decyzji histerezy |
| **Kill switch** | przycisk „Wyłącz ogrzewanie" (`/api/heating?disable=1`) z potwierdzeniem „Na pewno?" | ponowne włączenie tym samym przyciskiem (zielony „Włącz ogrzewanie") | najwyższy priorytet: przekaźnik OFF, stan `ST_IDLE`, ignoruje profil i BOOST; status pieca pokazuje szare koło z ✕ i podpis „Wyłączony" |
| **Wybieg pompy** | przejście `HEATING → OFF` przy `pump.enabled` | upływ `total_seconds` | stan `ST_PUMP_OVERRUN`: krótkie impulsy (`impulse_seconds` co `period_seconds`) rozpraszają ciepło resztkowe |
| **Tryb awaryjny cykliczny** | `emergency.enabled` | wyłączenie opcji | stan `ST_EMERGENCY_CYCLIC`: ON przez `on_seconds` co `period_seconds` niezależnie od czujników — ochrona przeciwzamrożeniowa gdy brak danych |
| **Awaryjne grzanie po awarii czujników** | `emergency_on_sensor_fault` + aktywne `FAULT_SENSOR_IFACE` | odzyskanie choć jednego czujnika (auto-clear awarii) | jak wyżej — duty cycle `on_seconds`/`period_seconds`, ale **warunkowo** (tylko na czas awarii, domyślnie WYŁ); bezwarunkowy `emergency.enabled` ma priorytet |
| **Tryb symulacji** | włączona symulacja czujników/ogrzewania | wyłączenie | stan `ST_SIMULATION`; decyzja ON/OFF liczona jak zwykle, ale GPIO nie jest sterowane (chyba że tryb mieszany) |

### D. Zdarzenia sieciowe i czasu (`network_manager.c`)

| Zdarzenie | Warunek | Zakończenie / akcje |
|---|---|---|
| **Połączenie STA** | tryb klient i podane SSID | po `IP_EVENT_STA_GOT_IP` ustawiany `network_up`, start SNTP |
| **Utrata STA** | `WIFI_EVENT_STA_DISCONNECTED` | auto-reconnect do 10 prób; po wyczerpaniu zgłoszony brak sieci (`FAULT_NETWORK`) |
| **Synchronizacja czasu (SNTP)** | uzyskanie IP w trybie STA | ustawia zegar rzeczywisty; poniżej `HE_TIME_VALID_EPOCH` czas jest „nieważny" i używany jest zegar wirtualny |
| **Reset sieci (przycisk)** | przytrzymanie BOOT (GPIO0) przez `HE_NET_RESET_HOLD_MS = 5 s` | `storage_reset_network` + restart AP z domyślnym SSID/hasłem; pozostała konfiguracja zachowana |

### E. Powiadomienia (`notification_manager.c`)

Powiadomienia są **odkładane do kolejki** (`notification_dispatch_alert` /
`notification_dispatch_restart`) i realizowane przez **osobne zadanie workera**
(`notify_worker_task`, prio 4, **nie subskrybowane task-watchdog**). Dzięki temu
blokujące I/O SMTP/SMS (DNS + TCP + handshake, rzędu sekund) **nigdy nie blokuje
pętli sterowania** ani nie trzyma `he_config_lock` — nawet przy wolnym /
nieosiągalnym serwerze nie ma ryzyka TWDT-reboot. Miejsca wyzwalania
(`fault_manager` przy awarii, `control_engine` 60 s po restarcie) kopiują potrzebne
pola pod lockiem i odkładają komendę bez blokowania (głębokość kolejki 2; pełna
→ porzucenie + log).

- **Warunek:** podniesienie awarii (`fault_manager`) → alert; uruchomienie
  (po 60 s real uptime) → restart. Pojedynczo na awarię (flaga `s_notified`).
- **Akcje:** e-mail przez SMTP (`email_enabled` + adres) i/lub SMS przez bramę
  HTTP (`sms_enabled` + telefon). Temat alertu: `[Heating] fault N`; temat
  restartu: `[nazwa urządzenia] RESTART` (non-ASCII → RFC 2047). Treść = opis
  awarii / szczegółowy raport restartu (patrz „Powiadomienie po restarcie").
  Błędy wysyłki są logowane, ale nie blokują sterowania.
- **Konfigurowane typy zdarzeń:** przełączniki w karcie „Powiadomienia"
  (podsekcja „Typy zdarzeń (e-mail)", `POST /api/notify/events`) decydują, czy
  dana kategoria generuje powiadomienie:
  - `notify_ev_faults` — powiadomienia o awariach (domyślnie **WŁ**),
  - `notify_ev_restart` — powiadomienie o resecie (domyślnie **WŁ**).
  Oba domyślnie WŁ zachowują dotychczasowe zachowanie (e-mail przy każdej
  awarii i przy resecie). Wyłączenie „awarii" wyłącza i e-mail, i SMS dla awarii
  (bramkowanie na poziomie dispatch, wspólnym dla obu kanałów). Pola są
  przechowywane w `system_config_t` (nie w `notify_cfg_t`); ponieważ `false` od
  upgrade byłby nieodróżnialny od „użytkownik wyłączył", stosujemy sentinel
  `notify_ev_ver`: przy pierwszym boot/upgrade `repair_config` ustawia oba na WŁ
  jednorazowo, a następnie ustawienia użytkownika są chronione (`ver=1`).
  Stan przełączników jest udostępniany w `/api/state` (`notify_ev`). Poza
  zakresem: powiadomienia o zmianach stanu i jakości czujników.
- **Echo ustawień w UI i ochrona haseł:** karta Powiadomienia ładuje bieżące
  wartości (odbiorca, serwer SMTP, użytkownik, telefon, brama SMS oraz
  przełączniki e-mail/SMS) z bloku `notify` w `/api/state`; karta Sieć ładuje
  bieżący SSID i tryb (klient/AP) z bloku `wifi`. **Hasła (SMTP, Wi-Fi) nie są
  nigdy udostępniane do przeglądarki** — pole hasła pozostaje puste, a pusta
  wartość przy zapisie (`POST /api/notify` / `POST /api/network`) oznacza
  „zachowaj bieżące hasło" (nadpisanie następuje tylko po wpisaniu niepustej
  wartości). Pozwala to zmienić np. odbiorcę bez ponownego wpisywania hasła.

#### Diagnostyka i testowanie e-mail (przycisk „Testuj e-mail")

Karta Powiadomienia zawiera przycisk **„Testuj e-mail"** — zapisuje aktualną
konfigurację SMTP, wysyła testową wiadomość i pokazuje **pełny log rozmowy
SMTP** (każda komenda `C:` i odpowiedź `S:` serwera). Dzięki temu widać
dokładnie, na którym etapie występuje problem.

**Ograniczenia implementacji SMTP:**
- ESP32 wysyła SMTP **bez szyfrowania** (plain-text TCP, zwykle port 25).
- **Nie obsługuje TLS/SSL** — publiczne serwery wymagające STARTTLS (Gmail,
  Outlook.com, WP, OVH, Home.pl) **nie zadziałają bezpośrednio**.
- Działa tylko w trybie **STA** (klient routera) — w trybie AP ESP32 nie ma
  dostępu do internetu.

**Rekomendowane rozwiązanie dla Gmaila/Outlooka:** lokalny pośrednik SMTP
(np. `msmtp` lub `postfix` na Raspberry Pi), który nasłuchuje plain-text
na porcie 25 i forwarduje przez TLS do właściwego serwera. W polu „Serwer
SMTP" w ESP32 wpisz adres IP tego pośrednika.

**Pole „Serwer SMTP"** akceptuje format `host` lub `host:port` (np.
`192.168.1.10:25`).

**Najczęstsze błędy (widoczne w logu diagnostycznym):**

| Komunikat | Przyczyna | Rozwiązanie |
|---|---|---|
| `FAIL: cannot resolve host` | Nieprawidłowa nazwa hosta lub brak DNS | Użyj adresu IP |
| `FAIL: connect refused/timeout` | Zły port, firewall lub serwer nie nasłuchuje | Sprawdź port (zwykle 25 dla plain-text) |
| `RECV failed (timeout/close)` | Serwer przerwał połączenie (często wymaga TLS) | Potrzebny pośrednik bez TLS |
| `OK: email accepted by server` | Sukces — mail dotarł do serwera SMTP | Sprawdź spam w skrzynce odbiorcy |

**Testowe powiadomienie** można też wywołać ręcznie przez
`POST /api/notify/test` (zwraca JSON `{"result":"..."}` z logiem SMTP).

## Tryb symulacji (spec pkt 8)

W UI oznaczony bannerem/kolorem. Możliwa symulacja pojedynczych czujników
(stała, narastanie, nagły spadek, brak odpowiedzi, poza zakresem, wirtualny)
oraz symulacja ogrzewania z modelem bezwładności cieplnej (skuteczne /
nieskuteczne / przegrzewanie). W symulacji fizyczne GPIO nie jest aktywne,
chyba że włączono tryb mieszany (laboratorium).

Opcja **przyspieszonego czasu ×10** (`sim_time_accel`) uruchamia całą pętlę
sterowania na wirtualnym zegarze 10×: model cieplny, liczniki BOOST / trybu
awaryjnego / wybiegu pompy, histereza i blokada antyoscylacyjna oraz zapis
próbek postępują 10× szybciej. Próbki otrzymują wirtualne znaczniki czasu
odstępniane co „minutę”, więc historia (a z nią wykres) zapełnia się i przewija
ok. 10× szybciej — pozwala to obserwować cykle grzania bez czekania i bez
sieci (SNTP). Współczynnik: `HE_SIM_TIME_SCALE` w `app_config.h`.

## GPIO

| Sygnał            | GPIO | Uwaga                       |
|-------------------|------|-----------------------------|
| Załączenie pieca  | 16   | linia do przekaźnika        |
| Reset sieci (btn) | 0    | BOOT, przytrzymanie 5 s     |
| Dioda LED         | 2    | wbudowana, miga podczas przytrzymania BOOT |
| UART czujników TX | 17   | zewn. interfejs czujników   |
| UART czujników RX | 18   |                             |

### Przycisk BOOT + dioda LED

Przytrzymanie przycisku BOOT (GPIO0) powoduje:
- **0–3 s**: wolne miganie diody LED (GPIO2, ~500 ms) — ostrzeżenie.
- **3–5 s**: szybkie miganie (~200 ms) — za chwilę reset.
- **≥5 s**: dioda świeci ciągle, **reset konfiguracji sieci** do AP
  `ESP` / `12345678` (pozostałe ustawienia zachowane), potem gaśnie.
Puszczenie przed upływem 5 s anuluje operację.

Przycisk EN na module to sprzętowy reset procesora — nie jest obsługiwany
programowo (podczas trzymania EN kod nie działa).

Protokół ramek czujników: `[0xAA][len][payload][CRC8][0x55]`; odpowiedź na
poll: `[count][id, t_hi, t_lo]...` (temperatura w setnych stopnia C).
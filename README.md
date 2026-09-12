# TASX Optimizer 🚀

> **Внимание:** Это не чит. Это не магия. Это просто код, который делает то, что Microsoft забыла сделать за 30 лет существования Windows.

TASX — это оптимизатор, созданный для борьбы с ужасной оптимизацией движка Roblox и его привычкой жрать память как не в себя. Если у вас лагает, а FPS стремится к нулю — возможно, вам сюда. Если нет — зачем вы здесь?

## 🤔 Что оно вообще делает?

TASX следит за процессами Roblox (через WMI, хуки и прочие страшные слова) и перераспределяет ресурсы системы так, чтобы ваша игра работала, а не умирала мучительной смертью.

### 🎯 Активное окно (то, в которое вы тыкаете мышкой)

- **Приоритет:** `HIGH_PRIORITY_CLASS` + полный доступ ко всем ядрам CPU.
- **Энергосбережение:** выключено — EcoQoS / power throttling off через `PROCESS_POWER_THROTTLING_STATE`.
- **I/O и память:** нормальный приоритет.
- **Таймер:** 0.5 мс для плавности кадров (потому что стандартные 15 мс — это для калькуляторов). Авто-отключение на фермах без фокуса — чистому фарму он не нужен.

### 📉 Фоновые окна (для тех, кто фармит на 10+ аккаунтах)

- **Приоритет:** `IDLE_PRIORITY_CLASS`, pinned на энергоэффективные E-ядра (гибридные CPU; иначе — нижняя половина ядер).
- **Режим эффективности:** Windows Efficiency Mode (EcoQoS только на уровне процесса, без перечисления потоков), VeryLow I/O, Low memory priority на фермах (`BackgroundMemPriority=2`, VeryLow в остальных случаях).
- **Очистка памяти (keep-hot, `FarmKeepHot=1`):** нефокусные клиенты фермы — РАБОТАЮЩИЕ, а не бездействующие. Периодический проход — только soft; жёсткий trim (`EmptyWorkingSet`) — только после `HardTrimAfterSec` (1800 с по умолчанию) непрерывной потери фокуса или при commit ≥90%. **Никогда для окна в фокусе.** Ниже `TrimSkipBelowMB` (250 по умолчанию) — пропуск (0 syscall'ов, из кэша). Лог раз в 30 с несёт `avg WS MB, commit%` для расчёта плотности фермы.
- **FarmBoost hotkey** (`BoostHotkey=Ctrl+Alt+B`, системный): одна клавиша переключает ВСЕ клиенты между `IDLE/E-ядра/EcoQoS` и полной мощью (`HIGH/все ядра`, EcoQoS off, обрезка приостановлена). Новые клиенты автоматически присоединяются к «горячей» стороне; переключение фокуса не понижает их, пока FarmBoost ON; `FarmBoostDefault=1` стартует горячим. `BoostHotkey=off` отключает (если комбинация занята макро-софтом).


### 🌍 Системные твики

- Отключение Game DVR и фоновой записи; MMCSS-профиль `Games` поднят; сетевой троттлинг выключен (`SystemResponsiveness=0`).
- GPU: high-performance preference для реального пути `RobloxPlayerBeta.exe` (fallback на имя exe).
- Питание: схема Ultimate/High Performance на время игры, восстановление после.
- Очистка Standby List при запуске Roblox (debounce 1/60 с для пачечных спавнов; требует админа). Система чистки разделена: `SystemCleanStandby=1` (безопасный reclaim page-cache) vs `SystemCleanEmptyWS=0` на фермах (глобальный empty working sets выселяет целые фермы — только opt-in).
- Убийство `RobloxCrashHandler.exe` постоянным свипом (job-child фильтр: убиваются только crash handlers).
- **FastFlags:** атомарная настройка `ClientAppSettings.json` для каждой установленной версии клиента (подробности в разделе «Под капотом»).

### 🆕 Умные фичи ферм

- **Farm auto-respawn** (`RespawnOnCrash=1`): неожиданно упавший клиент перезапускается той же командной строкой — с лимитом `RespawnPerHourMax` против crash-loop. Полностью event-driven: ни одного нового потока и ни одного цикла опроса.
- **Автоочистка кэша** (`CacheMaxGB=20`): LRU-чистка старых файлов кэша — кэш больше не разрастается на сотни ГБ за недели 24/7 фарма.
- **Watchdog зависших клиентов** (`HungClientWatch=1`): детект через `IsHungAppWindow` → лог + опционально kill/respawn (связка с auto-respawn).
- **CLI:** `TASX.exe --status` — one-shot сводка фермы (клиенты, RAM, commit %, соединения) и выход; `--preset farm20` — быстрое переключение пресета FastFlags в `TASX.ini`.
- **Таймстампы в логе** (`LogTimestamps=1`): каждая строка файлового лога со временем — разбор инцидентов больше не вслепую.

## 🤝 Совместимость с Инжекторами

Мы не враги. Мы коллеги.

| За что отвечаем мы (TASX) | За что отвечает Инжектор |
|---------------------------|--------------------------|
| Приоритеты CPU, Affinity, EcoQoS | Графика, качество текстур, лимит FPS |
| Приоритеты I/O и Памяти | Пер-клиентские настройки RAM |
| Mute Audio (WASAPI) | ... |
| Отключение телеметрии | ... |

По умолчанию (`InjectorOwnsGraphics=1`) TASX не трогает графические настройки, чтобы не ломать ваши красивые пресеты из инжектора.

**Правило владения ключами** (`Infra/fflags.cc: BuildFlagPlan`): графические ключи (`TargetFps`, renderer, lighting, texture, `GpuTextureCompressor`, `UseLevelOfDetail`) пропускаются при `InjectorOwnsGraphics=1`, если не задан `ForceGraphicsFlags=1`. Streaming/телеметрия пишутся всегда. `ApplyToVersion` всё равно выполняет атомарный read-modify-write (tmp-файл + `MoveFileEx`), сохраняя все ключи инжектора и пользователя.

**Дефолты:** старые конфиги без ключа сохраняют `InjectorOwnsGraphics=1` (прежнее поведение); новая ферма-конфиг `TASX.ini` поставляется с `InjectorOwnsGraphics=0` + `Preset=farm20` (графика под контролем TASX). С ферма-пресетом (`farm15|farm20|farm30|weak|balanced`) отсутствие ключа = `0`; без пресета = `1`.

## ⚠️ Что нужно знать перед запуском

1. **Админка обязательна.** Для твиков реестра (HKLM), очистки памяти и джобов нужны права администратора.
2. **Установка:** запустите `ScheduledTaskInstaller.bat` один раз. Это создаст задачу «TASX Agent», которая будет запускаться с повышенными привилегиями при старте системы.
3. **Удаление:** надоело? Запустите `Uninstall.bat`. Всё почистится.
4. **Один экземпляр:** нельзя запустить два TASX одновременно. Система не каменная, но и не бесконечная.

## 📥 Как скачать и использовать (для нормальных людей)

Забудьте про Discord, ссылки и долгие ожидания ответа от поддержки.

1. Идите в раздел **[Releases](https://github.com/Pozit1vchic/TASX-Win-Roblox-Optimizer/releases)** справа (или сверху, зависит от темы оформления GitHub).
2. Скачайте последний `.zip` архив.
3. Распакуйте.
4. Запустите `ScheduledTaskInstaller.bat` от имени администратора.
5. Готово. TASX теперь работает в фоне и делает вашу жизнь лучше.

## 💻 Для программистов (компиляция)

Если вы считаете, что можете сделать лучше (спойлер: вряд ли), вот как собрать проект:

**Visual Studio:** откройте `TASX.sln`. Debug — сборка с консолью и логами, Release — тихая сборка без окон.

**MSYS2 / MinGW (Makefile):**

- `mingw32-make` — консольная версия с логами (`TASX.exe`)
- `mingw32-make windows` — тихая GUI-версия
- `mingw32-make test` — юнит-тесты парсеров (config / fflags / respawn cmdline)
- `mingw32-make clean` — убрать за собой мусор

**build.bat** — сборка без make (нужен только g++ и windres из MSYS2 UCRT64; путь к тулчейну правится в шапке файла): `build.bat` — консольная, `build.bat silent` — GUI.

*Примечание:* `TASX.exe` должен лежать в одной папке с `.bat` файлами и `TASX.ini` (если он есть).

## 🧠 Как это работает под капотом (для гиков)

Архитектура построена на событиях, а не на тупых циклах опроса.

- **Infra/master.cpp** — оркестратор. Один поток, одна очередь событий. Горячая перезагрузка конфига по mtime каждые 10 секунд.
- **WMI.cc** — асинхронный наблюдатель за процессами. Никаких гонок данных.
- **jobs.cc** — управление группами процессов. Фокус определяется точно, без перемещения между джобами (чтобы не получить `ACCESS_DENIED`).
- **CPU.cc** — работа с топологией процессора через `ntsys.c`. Никакого хардкода.
- **trimmer.cc** — умная обрезка памяти. Никогда не трогает активное окно.
- **fflags.cc** — атомарная запись в `ClientAppSettings.json`. Сохраняет ваши личные флаги, добавляет наши.

Всего ~5 потоков независимо от количества запущенных клиентов Roblox. Эффективность? Да.

### Карта модулей

```
Infra/
  master.cpp   Оркестратор: одна типизированная очередь событий, которую
               кормят WMI, job completion port, foreground hook и
               low-memory reactor; однопоточный state machine через
               InitSubsystems()/ShutdownSubsystems() и Ctrl-handler
               (корректный выход, teardown power/Wait/Mutex); hot-reload
               TASX.ini по mtime; FocusDwellMs anti-flap (по умолчанию
               1800 мс, первый фокус мгновенный), предупреждение о
               pagefile-томе (PagefileWarnFreeGB), авто-создание TASX.ini
               с документированными дефолтами; FarmBoost toggle;
               respawn-очередь упавших клиентов
  WMI.cc       Асинхронный WMI-наблюдатель процессов с Indication-drain,
               PID — напрямую из TargetInstance.Handle
  jobs.cc      Единая фоновая cgroup (CPU/MEM caps фермы, KILL_ON_CLOSE
               под управлением KillOnAgentExit); JobAssignMode=auto|
               diagnose|off (force = deprecated alias), per-PID sticky
               fallback для foreign Job; фокус — per-process
               (priority/affinity/EcoQoS через CPU.cc, никогда cross-job
               move); IOCP фильтрует NEW_PROCESS только на
               robloxcrashhandler.exe
  CPU.cc       Нет своего состояния топологии — единственный источник
               истины ntsys.c (tasx_get_*_mask); per-process профиль
               планировщика + boost toggle
  trimmer.cc   Один поток-планировщик + min-heap дедлайнов: адаптивный
               интервал, skip-if-below-threshold, фокус не тримится,
               farm keep-hot; общий снапшот давления за 5 с; LowMem
               принадлежит master reactor
  stats.cc     Снапшот всех процессов ОДНИМ syscall'ом (NtQuerySystemInfo)
               + QueryProcessNameByPid для job-port фильтра
  winhook.cc   EVENT_SYSTEM_FOREGROUND hook (только уведомление; PID
               фокуса читается через GetForegroundWindow — единый механизм)
  hotkey.cc    Системный FarmBoost hotkey через RegisterHotKey на
               отдельном message-pump потоке (без окна/DLL/поллинга)
  ntsys.c      [C] ntdll/privilege слой + файловый лог (tasx_log, ротация
               1 МБ), P/E топология, commit charge, ETW (проверенные GUID)
  config.c     [C] Чтение TASX.ini с пропуском BOM + hot-reload
               (config_reload), авто-создание дефолтов
               (config_create_default), итератор записей для [FastFlags]
  tweaks.cc    One-shot реестровые твики + power scheme; UserGpuPreferences
               резолвит полный путь exe; HKLM под элевацией
  fflags.cc    Чтение/запись ClientAppSettings.json с escaped-quote
               парсингом и mtime+size+hash скипом (атомарный tmp +
               MoveFileEx, debounce 5 с + dirty flag); пресеты
               farm20/farm30(+farm15 alias)/weak/balanced из официального
               18-ключевого allowlist'а; anti-флаги,
               InjectorOwnsGraphics/ForceGraphicsFlags gate
```

**FastFlags подробнее:** пресеты собраны из официального allowlist'а Roblox (18 ключей, 09.2025+): живёт только картофель — `TextureQuality=0`, `FRMQuality 0`, grass-дистанции 0 + тихий воздух, CSG switching low, `PauseVoxelizer`, `SkyGray`, `MSAA=1`, `NoDPIScale`, `D3D11`. Мёртвые pre-allowlist флаги (D3D10, Voxel, streaming, telemetry, physics, LOD) НЕ пишутся; `DFIntTaskSchedulerTargetFps` остаётся как zero-cost placeholder FPS-интента. Сканы дебаунсятся 1/5 с с dirty-флагом; `FFlagsPruneDead=1` (ферма-дефолт) удаляет pre-allowlist остатки; hourly-строка `Effective set` показывает живые счётчики; неизвестные/мёртвые флаги фильтруются с one-time `LOGW` + подсказкой замены; анти-флаги (`Future`/`ShadowMap`, `TextureQuality 1-3`, `FRM>0`, `MSAA>1`, grass>0) принудительно сбиваются/удаляются. Презир: `[FastFlags] Preset=farm20|farm30|weak|balanced|off` + ручные `key=value` поверх; легаси `[Roblox]` секция сохранена для совместимости (D3D10→D3D11 fallback, Voxel→PauseVoxelizer+SkyGray).

### Логирование и hot-reload

Все лог-строки идут через `LOGI/W/E` и rate-limited'ятся. `[Log] LogFile` — файловый лог (1 МБ ротация в `.old`, одновременный stdout), `LogLevel=info|warn|error`. `LogTimestamps=1` добавляет время к каждой строке файлового лога. Hot-reload: mtime `TASX.ini` поллится каждые 10 с на главном цикле — правки пере-применяют FFlags/tweaks/лимиты джобов и пороги триммера без рестарта и новых потоков.
### Ключи фермы `TASX.ini`

`[TASX] JobAssignMode=auto|diagnose|off`, `FarmKeepHot=1`, `HardTrimAfterSec=1800`, `BackgroundMemPriority=2`, `TrimSkipBelowMB=250`, `SystemCleanStandby=1`, `SystemCleanEmptyWS=0`, `FocusDwellMs=1800` (легаси `FocusHysteresisMs` fallback), `PagefileWarnFreeGB=8`, `BackgroundCpuCapPercent=25`, `JobMemoryCapMB=8192` (per-process); `[FastFlags] Preset=farm20` + `FFlagsPruneDead=1` + ручные `key=value`; `[Roblox] UncapFps=0 TargetFps=20 Renderer=D3D10 Lighting=Voxel TextureQuality=0` (легаси, перекрывается пресетом; D3D10→D3D11, Voxel→PauseVoxelizer). Бюджетное правило: `N × avgWS < RAM × 0.75`, иначе keep-hot невозможен (rate-limited `LOGW`).

Новые ферма-ключи: `RespawnOnCrash=1`, `RespawnPerHourMax=6`, `HungClientWatch=1`, `HungKillAfterSecMin=60`, `CacheMaxGB=20`, `LogTimestamps=1`.

Проектировался под 100+ одновременных клиентов: нет polling-циклов на горячем пути (выходы, crash handlers, чистка памяти и discovery — событийные), ~5 потоков независимо от числа клиентов, один syscall для состояния всей системы.

## 📋 Требования

- **ОС:** Windows 10/11 (другие ОС не поддерживаются, извините, линуксоиды).
- **Права:** Администратор (для полной функциональности).

---

*Сделано с любовью, ненавистью к лагам и сарказмом.*

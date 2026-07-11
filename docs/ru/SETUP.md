# Установка сервера (RU)

Как собрать и запустить сервер TrinityCore **master** на ретейл-билде
**12.0.7.68275 (Midnight)**. Нужен собственный легально приобретённый
ретейл-клиент WoW.

> [English](../en/SETUP.md)

## 1. Требования

**Хост (рекомендуется Linux, напр. Debian/Ubuntu):**
- 8+ ядер CPU, **24 ГБ+ RAM** (ретейл-БД и карты тяжёлые), 200 ГБ+ диска
- MySQL 8.0 / MariaDB 10.6+
- `git`, `cmake`, `clang`/`gcc`, `boost` (1.78+), `openssl`, `zlib`, `readline`

Современный ретейл прожорлив. Дай машине swap и OOM-guard (`earlyoom`) — иначе
тяжёлая сборка или скачок нагрузки может подвесить VM.

## 2. Сборка ядра

```bash
git clone https://github.com/TrinityCore/TrinityCore.git
cd TrinityCore && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/tc -DTOOLS=1 -DWITH_WARNINGS=0
make -j$(nproc) && make install
```

> ⚠️ **Не** запускай тяжёлый `make -j` пока на той же машине работает живой
> worldserver — 60-секундный watchdog принудительно уронит его при нехватке CPU.
> Сначала сборка, потом запуск (или собирай на другой машине).

## 3. Извлечение игровых данных (из ТВОЕГО клиента)

Инструментами `mapextractor`, `vmap4extractor` + `vmap4assembler` и
`mmaps_generator` (собраны выше) запусти извлечение **внутри папки
ретейл-клиента**, чтобы получить `dbc/`, `maps/`, `vmaps/`, `mmaps/`,
`cameras/`. Скопируй их в `/opt/tc/data`.

Для актуального ретейла извлечение идёт из хранилища CASC. Эти данные —
собственность Blizzard, они остаются на твоей машине и никогда не публикуются.

## 4. Базы данных

Создай четыре схемы и пользователя:

```sql
CREATE DATABASE auth       DEFAULT CHARSET utf8mb4;
CREATE DATABASE characters DEFAULT CHARSET utf8mb4;
CREATE DATABASE world      DEFAULT CHARSET utf8mb4;
CREATE DATABASE hotfixes   DEFAULT CHARSET utf8mb4;
CREATE USER 'trinity'@'localhost' IDENTIFIED BY 'trinity';
GRANT ALL ON *.* TO 'trinity'@'localhost';
```

Импортируй базовые SQL TrinityCore для `auth`, `characters`, `hotfixes` и
**базу мира под билд 68275**. База мира собирается из твоих данных — в этом
репозитории её нет (см. [DISCLAIMER](../../DISCLAIMER.md)).

## 5. Конфигурация

Скопируй `worldserver.conf.dist` → `worldserver.conf` и `bnetserver.conf.dist` →
`bnetserver.conf` в `/opt/tc/etc`. Пропиши подключение к БД под пользователя
`trinity`. Укажи `DataDir` = `/opt/tc/data`. В `auth.realmlist` пропиши в `address`
IP твоего сервера.

## 6. Запуск

```bash
# bnetserver (авторизация battle.net) — напр. как systemd-юнит
/opt/tc/bin/bnetserver -c /opt/tc/etc/bnetserver.conf

# worldserver — первый запуск строит кэши, ~7 минут
cd /opt/tc/bin && ./worldserver -c /opt/tc/etc/worldserver.conf
```

Запускай `worldserver` в `tmux`/`screen`, чтобы сохранить консоль (нужна для
`reload`, `.account`, GM-команд). Для перезагрузок рекомендуется systemd-юнит,
стартующий его после MySQL.

Открытые порты: **8085** (мир), **1119** (bnet), **8081** (rest/login).

## 7. Применение community-фиксов

```bash
cd scripts
DB_USER=trinity DB_PASS=trinity DB_NAME=world ./apply_fixes.sh
```

Затем в консоли worldserver: `reload quest_template` (квест-фиксы применяются
вживую, без рестарта). Изменения объектов / кладбищ / привязок рейдов требуют
рестарта. См. [FIXES.md](FIXES.md).

## 8. Создание аккаунта

В консоли worldserver:

```
account create <имя> <пароль>
account set gmlevel <имя> 3 -1
```

Дальше — **[CONNECT.md](CONNECT.md)** для патча и настройки клиента.

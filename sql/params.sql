-- Default settings of the X6100 GUI: run by the build (sql/CMakeLists.txt)
-- into params.db, installed as /usr/share/x6100/params.default.db.
-- The GUI moves it forward with its own migrations (src/params/migrations.c),
-- so a change here only reaches a radio with a fresh /mnt/params.db.
--
-- Copyright (c) 2026 Giovanni Ricciardi IU1WXY
--
-- Frequencies in Hz. Modes by number, as the radio control library counts
-- them: 0 LSB, 1 LSB-D, 2 USB, 3 USB-D, 4 CW, 5 CWR, 6 AM, 7 NFM.
-- Row order matters for the rowids: add new rows at the end of a table.

-- Band list. type: 1 = active (band keys step through these), 0 = inactive.
-- id 1..99 ham, 100.. broadcast, 200.. CB, 300.. transverter.
CREATE TABLE bands (id INTEGER PRIMARY KEY, name TEXT, start_freq INTEGER, stop_freq INTEGER, type INTEGER);
INSERT INTO bands VALUES
    (1,   '160m',     1810000,   2000000, 1),
    (2,   '80m',      3500000,   3800000, 1),
    (3,   '40m CW',   7000000,   7040000, 1),
    (4,   '40m SSB',  7040000,   7200000, 1),
    (5,   '30m',     10100000,  10150000, 1),
    (6,   '20m CW',  14000000,  14070000, 1),
    (7,   '20m SSB', 14070000,  14350000, 1),
    (8,   '17m',     18068000,  18168000, 1),
    (9,   '15m',     21000000,  21450000, 1),
    (10,  '12m',     24890000,  24990000, 1),
    (11,  '10m',     28000000,  29700000, 1),
    (12,  '6m',      50000000,  53900000, 1),
    (201, 'CB A',    26055000,  26295000, 1),
    (202, 'CB B',    26515000,  26745000, 1),
    (203, 'CB C',    26965000,  27195000, 1),
    (204, 'CB D',    27415000,  27645000, 1),
    (100, 'Broadcast 75m',  3900000,  4000000, 0),
    (101, 'Broadcast 60m',  4750000,  5060000, 0),
    (102, 'Broadcast 49m',  5850000,  6350000, 0),
    (103, 'Broadcast 41m',  7200000,  7500000, 0),
    (104, 'Broadcast 31m',  9400000,  9990000, 0),
    (105, 'Broadcast 25m', 11600000, 12100000, 0),
    (106, 'Broadcast 22m', 13500000, 13870000, 0),
    (107, 'Broadcast 19m', 15100000, 15600000, 0),
    (108, 'Broadcast 16m', 17550000, 18050000, 0),
    (109, 'Broadcast 15m', 18900000, 19020000, 0),
    (110, 'Broadcast 13m', 21450000, 21850000, 0),
    (111, 'Broadcast 11m', 25600000, 26055000, 0),
    (300, '4m',      70000000,  72000000, 1),
    (301, '2m',     144000000, 150000000, 1),
    (302, '70cm',   432000000, 438000000, 1),
    (13,  '60m',      5351500,   5366500, 1);

-- Where each band opens the first time: VFO A frequency and mode.
-- Broadcast bands and 4m have none. 60 m opens in USB, the sideband used there.
CREATE TABLE band_params (bands_id INTEGER, name TEXT, val INTEGER,
    UNIQUE (bands_id, name) ON CONFLICT REPLACE);
INSERT INTO band_params VALUES
    (1,   'vfoa_freq',   1810000), (1,   'vfoa_mode', 0),
    (2,   'vfoa_freq',   3500000), (2,   'vfoa_mode', 0),
    (3,   'vfoa_freq',   7000000), (3,   'vfoa_mode', 4),
    (4,   'vfoa_freq',   7040000), (4,   'vfoa_mode', 0),
    (5,   'vfoa_freq',  10100000), (5,   'vfoa_mode', 2),
    (6,   'vfoa_freq',  14000000), (6,   'vfoa_mode', 4),
    (7,   'vfoa_freq',  14100000), (7,   'vfoa_mode', 2),
    (8,   'vfoa_freq',  18068000), (8,   'vfoa_mode', 2),
    (9,   'vfoa_freq',  21000000), (9,   'vfoa_mode', 2),
    (10,  'vfoa_freq',  24890000), (10,  'vfoa_mode', 2),
    (11,  'vfoa_freq',  28000000), (11,  'vfoa_mode', 2),
    (12,  'vfoa_freq',  50000000), (12,  'vfoa_mode', 2),
    (201, 'vfoa_freq',  26055000), (201, 'vfoa_mode', 6),
    (202, 'vfoa_freq',  26515000), (202, 'vfoa_mode', 6),
    (203, 'vfoa_freq',  26965000), (203, 'vfoa_mode', 6),
    (204, 'vfoa_freq',  27415000), (204, 'vfoa_mode', 6),
    (301, 'vfoa_freq', 145500000), (301, 'vfoa_mode', 7),
    (302, 'vfoa_freq', 433500000), (302, 'vfoa_mode', 7),
    (13,  'vfoa_freq',   5360000), (13,  'vfoa_mode', 2);

-- General settings by name; those not listed take the GUI's own defaults.
CREATE TABLE params (name TEXT PRIMARY KEY ON CONFLICT REPLACE, val INTEGER);
INSERT INTO params VALUES
    ('band', 5),
    ('vol', 30),
    ('spectrum_factor', 1),
    ('spectrum_beta', 70);

-- Per mode: filter edges in Hz and tuning step in Hz.
CREATE TABLE mode_params (mode INTEGER, name TEXT, val INTEGER,
    UNIQUE (mode, name) ON CONFLICT REPLACE);
INSERT INTO mode_params VALUES
    (0, 'filter_low', 100), (0, 'filter_high', 2900), (0, 'freq_step', 500),
    (1, 'filter_low', 100), (1, 'filter_high', 2900), (1, 'freq_step', 500),
    (2, 'filter_low', 100), (2, 'filter_high', 2900), (2, 'freq_step', 500),
    (3, 'filter_low', 100), (3, 'filter_high', 2900), (3, 'freq_step', 500),
    (4, 'filter_low', 400), (4, 'filter_high',  800), (4, 'freq_step', 100),
    (5, 'filter_low', 400), (5, 'filter_high',  800), (5, 'freq_step', 100),
    (6, 'filter_low',   0), (6, 'filter_high', 4000), (6, 'freq_step', 1000),
    (7, 'filter_low',   0), (7, 'filter_high', 4000), (7, 'freq_step', 1000);

-- Tables that start empty: tuner memory per antenna and frequency, memory
-- channels, CW messages, transverter settings
CREATE TABLE atu (ant INTEGER, freq INTEGER, val INTEGER,
    UNIQUE (ant, freq) ON CONFLICT REPLACE);
CREATE TABLE memory (id INTEGER, name TEXT, val INTEGER,
    UNIQUE (id, name) ON CONFLICT REPLACE);
CREATE TABLE msg_cw (id INTEGER PRIMARY KEY, val INTEGER);
CREATE TABLE transverter (id INTEGER, name TEXT, val INTEGER,
    UNIQUE (id, name) ON CONFLICT REPLACE);

-- Database layout version, the starting point of the GUI's migrations
CREATE TABLE version (id INT NOT NULL DEFAULT 0);
INSERT INTO version (id) VALUES (1);

-- Dial frequencies of the digital modes (DigitalModesTable in src/cfg/db.h).
-- type: 0 FT8, 1 FT4, 2 JS8, 3 GhostNet, 4 WSPR, 5 FT2. mode 3 = USB-D.
CREATE TABLE digital_modes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    label varchar(64) NOT NULL,
    freq INTEGER NOT NULL CHECK(freq > 0),
    mode INTEGER NOT NULL DEFAULT 3 CHECK(mode >= 0 AND mode <= 7),
    type INTEGER NOT NULL,
    CONSTRAINT freq_type_uniq UNIQUE(freq, type));
CREATE INDEX digital_modes_type_idx ON digital_modes (type);
CREATE INDEX digital_modes_freq_idx ON digital_modes (freq);
INSERT INTO digital_modes VALUES
    (1,  'FT8 160m',  1840000, 3, 0),
    (2,  'FT8 80m',   3573000, 3, 0),
    (3,  'FT8 60m',   5357000, 3, 0),
    (4,  'FT8 40m',   7074000, 3, 0),
    (5,  'FT8 30m',  10136000, 3, 0),
    (6,  'FT8 20m',  14074000, 3, 0),
    (7,  'FT8 17m',  18100000, 3, 0),
    (8,  'FT8 15m',  21074000, 3, 0),
    (9,  'FT8 12m',  24915000, 3, 0),
    (10, 'FT8 10m',  28074000, 3, 0),
    (11, 'FT8 6m',   50313000, 3, 0),
    (12, 'FT4 80m',   3575000, 3, 1),
    (13, 'FT4 40m',   7047500, 3, 1),
    (14, 'FT4 30m',  10140000, 3, 1),
    (15, 'FT4 20m',  14080000, 3, 1),
    (16, 'FT4 17m',  18104000, 3, 1),
    (17, 'FT4 15m',  21140000, 3, 1),
    (18, 'FT4 12m',  24919000, 3, 1),
    (19, 'FT4 10m',  28180000, 3, 1),
    (20, 'FT4 6m',   50318000, 3, 1),
    (34, 'WSPR 160m', 1836600, 3, 4),
    (35, 'WSPR 80m',  3568600, 3, 4),
    (36, 'WSPR 60m',  5287200, 3, 4),
    (37, 'WSPR 40m',  7038600, 3, 4),
    (38, 'WSPR 30m', 10138700, 3, 4),
    (39, 'WSPR 20m', 14095600, 3, 4),
    (40, 'WSPR 17m', 18104600, 3, 4),
    (41, 'WSPR 15m', 21094600, 3, 4),
    (42, 'WSPR 12m', 24924600, 3, 4),
    (43, 'WSPR 10m', 28124600, 3, 4),
    (44, 'WSPR 6m',  50293000, 3, 4),
    (45, 'FT2 160m',  1843000, 3, 5),
    (46, 'FT2 80m',   3568000, 3, 5),
    (47, 'FT2 60m',   5360000, 3, 5),
    (48, 'FT2 40m',   7062000, 3, 5),
    (49, 'FT2 30m',  10144000, 3, 5),
    (50, 'FT2 20m',  14084000, 3, 5),
    (51, 'FT2 17m',  18108000, 3, 5),
    (52, 'FT2 15m',  21144000, 3, 5),
    (53, 'FT2 12m',  24923000, 3, 5),
    (54, 'FT2 10m',  28184000, 3, 5),
    (55, 'FT2 6m',   50316000, 3, 5),
    (21, 'JS8 160m',  1842000, 3, 2),
    (22, 'JS8 80m',   3578000, 3, 2),
    (23, 'JS8 40m',   7078000, 3, 2),
    (24, 'JS8 30m',  10130000, 3, 2),
    (25, 'JS8 20m',  14078000, 3, 2),
    (26, 'JS8 17m',  18104000, 3, 2),
    (27, 'JS8 15m',  21078000, 3, 2),
    (28, 'JS8 12m',  24922000, 3, 2),
    (29, 'JS8 10m',  28078000, 3, 2),
    (30, 'JS8 6m',   50318000, 3, 2),
    (31, 'GhostNet 80m',  3575000, 3, 3),
    (32, 'GhostNet 40m',  7107000, 3, 3),
    (33, 'GhostNet 20m', 14107000, 3, 3);

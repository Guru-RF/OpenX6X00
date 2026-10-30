"""Paths of the radio; __main__ sets the first ones from the command line."""
FILEBROWSER_PATH = "/mnt"           # root of the Files page (--filebrowser-path)
DB_PATH = ""
CLUSTER_CONF = "/mnt/cluster.conf"
TCI_CONF = "/mnt/tci.conf"
CHANNELS_FILE = "/mnt/channels.json"
QSO_LOG_DB = "/mnt/qso_log.db"
CW_TRAINER_FILE = "/mnt/cw_trainer.json"
VOICE_DIR = "/mnt/msg"
MACRO_FILES = {"psk": "/mnt/psk_macros.txt", "olivia": "/mnt/olivia_macros.txt",
               "jtty": "/mnt/jtty_macros.txt"}   # src/psk_macro.c, olivia_macro.c, jtty_macro.c
JS8_TEXTS_FILE = "/mnt/js8_texts.txt"   # INFO, STATUS and more, src/dialog_js8.c
WIFI_DIR = "/etc/NetworkManager/system-connections"
BACKUP_WORK = "/mnt/backup_work"    # uploaded backup, unpacked
BACKUP_SAFETY = "/mnt/backup_before_restore"
GUI_INIT = "/etc/init.d/S95gui"
CFG_SOCK = "/tmp/x6100_cfg.sock"   # the GUI's configurator socket (src/cfg_socket.cpp): Display, Ethernet pages
FIELD_DIR = "/mnt/field"            # refs.tsv, qrz.conf, logs/*.adi (rootfs/usr/sbin/x6100-field)
FIELD_CMD = "x6100-field"
WAVELOG_CMD = "x6100-wavelog"       # the Wavelog upload (rootfs/usr/sbin), its settings in FIELD_DIR/wavelog.conf
HTTPS_PORT = 0                      # set when the HTTPS server runs (the Gallery's in-browser conversion needs it)
HTTP_PORT = 80                      # the plain server: every other page
REMOTE_CONF = "/mnt/remote.conf"   # the browser remote (src/remote_server.c): enabled=, port=
WSS_REMOTE_PORT = 0                 # TLS front doors to the remote and TCI WebSockets for the https page (0: none)
WSS_TCI_PORT = 0
FUNNEL_DIR = "/run/x6100-funnel"    # the Panel and the Screen page for Tailscale Funnel, written at the start (#58)
LOCALTIME = "/etc/localtime"        # Time page: the timezone link
ZONEINFO = "/usr/share/zoneinfo"

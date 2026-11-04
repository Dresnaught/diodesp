import dbus
import serial
import threading
import time
import atexit
import io
import os
import json
import re
import yaml
import urllib.parse
import urllib.request
import unicodedata
from PIL import Image
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

# ur UeSBe serial MCU (Native USB / JTAG port on ESP32-S3)
SERIAL_COM = '/dev/ttyACM0'
baud = 115200
ser = None
ser_lock = threading.Lock()
last_art_url = None
ART_IMG_DIM = 160

# lyrics state and cache
lyrics_data = None
lyrics_view_active = False
current_lyric_tuple = None
last_lyrics_track = (None, None)
lyrics_lock = threading.Lock()

CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache_lyric")
os.makedirs(CACHE_DIR, exist_ok=True)

LOCAL_LYRICS_DIRS = [
    CACHE_DIR,
    os.path.expanduser("~/.lyrics")
]

def clean_mpris_text(text):
    if not text:
        return ""
    # Normalize common typographic punctuation that aren't in ASCII
    replacements = {
        '\u2018': "'", '\u2019': "'", '\u201a': "'", '\u201b': "'",
        '\u201c': '"', '\u201d': '"', '\u201e': '"', '\u201f': '"',
        '\u2013': "-", '\u2014': "-", '\u2015': "-",
        '\u2026': "...",
        '\u00a0': " ",
        '〜': "~",
        '・': "·",
        '【': "[", '】': "]",
        '（': "(", '）': ")",
        '「': '"', '」': '"',
        '『': '"', '』': '"',
        '［': '[', '］': ']',
    }
    for k, v in replacements.items():
        text = text.replace(k, v)
    text = unicodedata.normalize('NFKC', text)
    return text.strip()

try:
    ser = serial.Serial(SERIAL_COM, baud, timeout=0.3)
    print(f"connected to mcu(esp32) to {SERIAL_COM}")
except Exception as e:
    print(f"failed. No such device >:( . {e}, try again.")
    exit(1)

def send_to_mcu(msg):
    if not ser or not ser.is_open:
        return
    try:
        with ser_lock:
            ser.write(msg.encode('utf-8'))
            ser.flush()
        print(f"-> MCU: {msg.strip()}")
    except Exception as e:
        print(f"nu uh failed to send to mcu: {e}")

def convert_art_to_rgb565(art_url):
    try:
        if not art_url:
            return None
        if art_url.startswith('file://'):
            file_path = urllib.parse.unquote(urllib.parse.urlparse(art_url).path)
            img = Image.open(file_path)
        elif art_url.startswith('http://') or art_url.startswith('https://'):
            req = urllib.request.Request(art_url, headers={'User-Agent': 'Mozilla/5.0'})
            with urllib.request.urlopen(req, timeout=3) as resp:
                img = Image.open(io.BytesIO(resp.read()))
        else:
            img = Image.open(art_url)

        img = img.convert("RGB")
        w, h = img.size
        min_dim = min(w, h)
        left = (w - min_dim) // 2
        top = (h - min_dim) // 2
        img = img.crop((left, top, left + min_dim, top + min_dim))
        img = img.resize((ART_IMG_DIM, ART_IMG_DIM), Image.Resampling.LANCZOS)

        raw = img.tobytes()
        out = bytearray(ART_IMG_DIM * ART_IMG_DIM * 2)
        j = 0
        for i in range(0, len(raw), 3):
            r, g, b = raw[i], raw[i+1], raw[i+2]
            val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[j] = val & 0xFF
            out[j+1] = (val >> 8) & 0xFF
            j += 2
        return bytes(out)
    except Exception as e:
        print(f"error processing album art from {art_url}: {e}")
        return None

def send_image_to_mcu(raw_data):
    if not ser or not ser.is_open or not raw_data:
        return
    try:
        with ser_lock:
            header = f"IMG:{len(raw_data)}\n".encode('ascii')
            ser.write(header)
            ser.flush()
            chunk_size = 2048
            for i in range(0, len(raw_data), chunk_size):
                ser.write(raw_data[i:i+chunk_size])
            ser.flush()
        print(f"-> MCU: Sent album art ({len(raw_data)} bytes)")
    except Exception as e:
        print(f"failed to send image to mcu: {e}")

def sync_art(art_url):
    global last_art_url
    if art_url == last_art_url:
        return
    last_art_url = art_url
    if art_url:
        print(f"mpris: fetching album art from {art_url}")
        data = convert_art_to_rgb565(art_url)
        if data:
            send_image_to_mcu(data)
        else:
            send_to_mcu("IMG:CLEAR\n")
    else:
        send_to_mcu("IMG:CLEAR\n")

def clean_lyric_line(txt):
    if not txt:
        return ""
    txt = clean_mpris_text(txt)
    return txt.replace('|', '/').replace('\n', ' ').replace('\r', '').strip()

def parse_lrc(lrc_text):
    lines = []
    pattern = re.compile(r'^\[(\d+):(\d+(?:\.\d+)?)\](.*)$')
    for raw_line in lrc_text.splitlines():
        raw_line = raw_line.strip()
        m = pattern.match(raw_line)
        if m:
            mins = int(m.group(1))
            secs = float(m.group(2))
            start_ms = int((mins * 60 + secs) * 1000)
            txt = m.group(3).strip()
            if txt:
                lines.append({'text': txt, 'start_ms': start_ms, 'end_ms': None, 'words': []})
    lines.sort(key=lambda x: x['start_ms'])
    for i in range(len(lines)):
        if i + 1 < len(lines):
            lines[i]['end_ms'] = lines[i + 1]['start_ms']
        else:
            lines[i]['end_ms'] = lines[i]['start_ms'] + 5000
    return lines

def parse_yaml_lyrics(yaml_text):
    try:
        doc = yaml.safe_load(yaml_text)
        if isinstance(doc, dict) and 'lines' in doc:
            lines = []
            for item in doc['lines']:
                txt = str(item.get('text', '')).strip()
                if not txt:
                    continue
                start_ms = int(item.get('start_ms', 0))
                end_ms = int(item.get('end_ms', start_ms + 4000)) if 'end_ms' in item else None
                words = item.get('words', [])
                lines.append({
                    'text': txt,
                    'start_ms': start_ms,
                    'end_ms': end_ms,
                    'words': words
                })
            lines.sort(key=lambda x: x['start_ms'])
            return lines
    except Exception as e:
        print(f"lyrics: error parsing yaml: {e}")
    return None

def fetch_online_lyrics(title, artist, duration_sec=0):
    try:
        params = {'track_name': title, 'artist_name': artist}
        if duration_sec > 0:
            params['duration'] = round(duration_sec)
        query_str = urllib.parse.urlencode(params)
        url = f"https://lrclib.net/api/get?{query_str}"
        req = urllib.request.Request(url, headers={'User-Agent': 'DiodESP-Lyrics/1.0'})
        with urllib.request.urlopen(req, timeout=4) as resp:
            data = json.loads(resp.read().decode('utf-8'))
            if data.get('syncedLyrics'):
                return parse_lrc(data['syncedLyrics'])
    except urllib.error.HTTPError as e:
        if e.code != 404:
            print(f"lyrics: lrclib get code {e.code}")
    except Exception as e:
        print(f"lyrics: lrclib get error {e}")

    try:
        q = urllib.parse.quote(f"{title} {artist}")
        url = f"https://lrclib.net/api/search?q={q}"
        req = urllib.request.Request(url, headers={'User-Agent': 'DiodESP-Lyrics/1.0'})
        with urllib.request.urlopen(req, timeout=4) as resp:
            results = json.loads(resp.read().decode('utf-8'))
            if isinstance(results, list):
                for item in results:
                    if item.get('syncedLyrics'):
                        return parse_lrc(item['syncedLyrics'])
    except Exception as e:
        print(f"lyrics: lrclib search error {e}")

    return None

def fetch_lyrics(title, artist, duration_sec=0, track_url=None):
    if track_url and track_url.startswith('file://'):
        try:
            local_path = urllib.parse.unquote(urllib.parse.urlparse(track_url).path)
            base_path = os.path.splitext(local_path)[0]
            for ext in ['.lyricsfile.yaml', '.yaml', '.yml']:
                cand = base_path + ext
                if os.path.exists(cand):
                    with open(cand, 'r', encoding='utf-8') as f:
                        lines = parse_yaml_lyrics(f.read())
                        if lines:
                            print(f"lyrics: loaded local yaml from {cand}")
                            return lines
            cand = base_path + '.lrc'
            if os.path.exists(cand):
                with open(cand, 'r', encoding='utf-8') as f:
                    lines = parse_lrc(f.read())
                    if lines:
                        print(f"lyrics: loaded local lrc from {cand}")
                        return lines
        except Exception as e:
            print(f"lyrics: local file check error {e}")

    safe_title = "".join(c for c in title if c.isalnum() or c in " ._-").strip()
    safe_artist = "".join(c for c in artist if c.isalnum() or c in " ._-").strip()

    for d in LOCAL_LYRICS_DIRS:
        if not os.path.exists(d):
            continue
        cand_names = [
            f"{artist} - {title}.lyricsfile.yaml",
            f"{safe_artist} - {safe_title}.lyricsfile.yaml",
            f"{artist} - {title}.yaml",
            f"{safe_artist} - {safe_title}.yaml",
            f"{artist} - {title}.lrc",
            f"{safe_artist} - {safe_title}.lrc",
            f"{title}.lyricsfile.yaml",
            f"{safe_title}.lyricsfile.yaml",
            f"{title}.yaml",
            f"{title}.lrc"
        ]
        for name in cand_names:
            full = os.path.join(d, name)
            if os.path.exists(full):
                try:
                    with open(full, 'r', encoding='utf-8') as f:
                        content = f.read()
                        if name.endswith('.yaml') or name.endswith('.yml'):
                            lines = parse_yaml_lyrics(content)
                        else:
                            lines = parse_lrc(content)
                        if lines:
                            print(f"lyrics: loaded local lyrics from {full}")
                            return lines
                except Exception:
                    pass

    print(f"lyrics: fetching from lrclib for '{title} - {artist}'")
    lines = fetch_online_lyrics(title, artist, duration_sec)
    if lines:
        print(f"lyrics: fetched {len(lines)} lines from lrclib")
        try:
            cache_file = os.path.join(CACHE_DIR, f"{safe_artist} - {safe_title}.lyricsfile.yaml")
            yaml_content = {
                'version': '1.0',
                'metadata': {
                    'title': title,
                    'artist': artist,
                    'duration_ms': round(duration_sec * 1000)
                },
                'lines': lines
            }
            with open(cache_file, 'w', encoding='utf-8') as f:
                yaml.dump(yaml_content, f, allow_unicode=True, sort_keys=False)
            print(f"lyrics: cached to {cache_file}")
        except Exception as e:
            print(f"lyrics: cache write error {e}")
        return lines

    return None

def update_lyrics_at_pos(pos_ms):
    global current_lyric_tuple, lyrics_data
    with lyrics_lock:
        lines = lyrics_data

    if lines is None:
        return
    if len(lines) == 0:
        new_tuple = ("", "No lyric", "")
        if new_tuple != current_lyric_tuple:
            current_lyric_tuple = new_tuple
            send_to_mcu("LYRIC:|No lyric|\n")
        return

    n = len(lines)
    if pos_ms < lines[0]['start_ms']:
        prev = ""
        curr = "♪ ... ♪"
        next_txt = lines[0]['text']
    else:
        idx = 0
        for i in range(n):
            if lines[i]['start_ms'] <= pos_ms:
                idx = i
            else:
                break
        curr_line = lines[idx]
        if curr_line.get('end_ms') and pos_ms >= curr_line['end_ms']:
            prev = curr_line['text']
            curr = "♪ ... ♪"
            next_txt = lines[idx + 1]['text'] if idx + 1 < n else ""
        else:
            prev = lines[idx - 1]['text'] if idx > 0 else ""
            curr = curr_line['text']
            next_txt = lines[idx + 1]['text'] if idx + 1 < n else ""

    new_tuple = (prev, curr, next_txt)
    if new_tuple != current_lyric_tuple:
        current_lyric_tuple = new_tuple
        clean_prev = clean_lyric_line(prev)
        clean_curr = clean_lyric_line(curr)
        clean_next = clean_lyric_line(next_txt)
        send_to_mcu(f"LYRIC:{clean_prev}|{clean_curr}|{clean_next}\n")
        print(f"lyrics: -> MCU: {clean_curr}")

def poll_and_update_lyrics():
    with lyrics_lock:
        lines = lyrics_data
    if lines is None:
        send_to_mcu("LYRIC:|Searching lyrics...|\n")
        return
    if len(lines) == 0:
        send_to_mcu("LYRIC:|No lyric|\n")
        return
    try:
        get = get_mpris()
        if get:
            props = dbus.Interface(get, 'org.freedesktop.DBus.Properties')
            pos_us = int(props.Get('org.mpris.MediaPlayer2.Player', 'Position'))
            update_lyrics_at_pos(pos_us // 1000)
        else:
            update_lyrics_at_pos(0)
    except Exception as e:
        print(f"lyrics: poll error {e}")

def fetch_lyrics_worker(title, artist, duration_sec, track_url):
    global lyrics_data
    lines = fetch_lyrics(title, artist, duration_sec, track_url)
    with lyrics_lock:
        lyrics_data = lines if lines is not None else []
    if lines:
        poll_and_update_lyrics()
    else:
        print(f"lyrics: no lyrics found for {title} - {artist}")
        send_to_mcu("LYRIC:|No lyric|\n")

def sync_lyrics_track(title, artist, duration_sec, track_url):
    global last_lyrics_track, lyrics_data, current_lyric_tuple
    if (title, artist) == last_lyrics_track:
        return
    last_lyrics_track = (title, artist)
    with lyrics_lock:
        lyrics_data = None
        current_lyric_tuple = None

    send_to_mcu("LYRIC:|Searching lyrics...|\n")
    threading.Thread(target=fetch_lyrics_worker, args=(title, artist, duration_sec, track_url), daemon=True).start()

def cleanup():
    print("cleaning up and notifying MCU...")
    try:
        send_to_mcu("HOST:DISCONNECTED\n")
        time.sleep(0.05)
        if ser and ser.is_open:
            ser.close()
    except Exception:
        pass

atexit.register(cleanup)

DBusGMainLoop(set_as_default=True)
ambus = dbus.SessionBus()

def get_mpris():
    for service in ambus.list_names():
        if service.startswith('org.mpris.MediaPlayer2'):
            try:
                return ambus.get_object(service, '/org/mpris/MediaPlayer2')
            except Exception:
                pass
    return None

def send_cum(a):
    get = get_mpris()
    if not get:
        print("no mpris player found :(")
        return
    try:
        get_dbus = dbus.Interface(get, 'org.mpris.MediaPlayer2.Player')
        match a:
            case 0:
                get_dbus.PlayPause()
                print("mpris: toggled play/pause")
            case 1:
                get_dbus.Next()
                print("mpris: skipped next")
            case 2:
                get_dbus.Previous()
                print("mpris: skipped previous")
    except Exception as e:
        print(f"error sending mpris command: {e}")

def listen():
    while True:
        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if not line:
                    continue
                print(f"<- MCU: {line}")
                match line:
                    case "pause:play":
                        print("recieve pause/play from MCU, gonna do pause/continue")
                        send_cum(0)
                    case "next":
                        print("recieve next from MCU, gonna continue")
                        send_cum(1)
                    case "previous":
                        print("recieve previous from MCU: gonna go back")
                        send_cum(2)
                    case "QUERY" | "PING":
                        print("<- MCU: queried playing status -> re-syncing full state")
                        current_lyric_tuple = None
                        sync_initial_mpris()
                    case "lyrics:open" | "lyrics":
                        print("<- MCU: opened lyrics view -> sync lyrics")
                        lyrics_view_active = True
                        current_lyric_tuple = None
                        poll_and_update_lyrics()
                    case "lyrics:close":
                        print("<- MCU: lyrics view closed")
                        lyrics_view_active = False
                    case _:
                        print(f"unknown mcu line: {line}")
            else:
                time.sleep(0.02)
        except Exception as e:
            print(f"error read serial {e}")
            break

threading.Thread(target=listen, daemon=True).start()

had_player = False

# Periodic progress bar updater (every 1 second)
missed_player_count = 0

def progress_loop():
    global had_player, missed_player_count, last_art_url
    last_sec_tick = 0
    while True:
        try:
            get = get_mpris()
            if get:
                if not had_player:
                    print("mpris player detected, syncing track...")
                    had_player = True
                    sync_initial_mpris()
                had_player = True
                missed_player_count = 0

                props = dbus.Interface(get, 'org.freedesktop.DBus.Properties')
                status = str(props.Get('org.mpris.MediaPlayer2.Player', 'PlaybackStatus'))
                if status == 'Playing':
                    metadata = props.Get('org.mpris.MediaPlayer2.Player', 'Metadata')
                    pos_us = int(props.Get('org.mpris.MediaPlayer2.Player', 'Position'))
                    len_us = int(metadata.get('mpris:length', 0))

                    pos_ms = pos_us // 1000
                    cur_sec = pos_ms // 1000
                    total_sec = len_us // 1000000 if len_us > 0 else 0

                    now = time.time()
                    if now - last_sec_tick >= 1.0:
                        last_sec_tick = now
                        send_to_mcu(f"POS:{cur_sec}:{total_sec}\n")

                    update_lyrics_at_pos(pos_ms)
            else:
                if had_player:
                    missed_player_count += 1
                    # Require 4 consecutive failures before assuming player closed
                    if missed_player_count >= 4:
                        had_player = False
                        missed_player_count = 0
                        print("media player closed, resetting MCU to waiting for MPRIS...")
                        last_art_url = None
                        send_to_mcu("TITLE:Waiting for MPRIS...\n")
                        send_to_mcu("ARTIST:No active media player\n")
                        send_to_mcu("STATUS:Idle\n")
                        send_to_mcu("POS:0:0\n")
                        send_to_mcu("IMG:CLEAR\n")
                        send_to_mcu("LYRIC:|No lyric|\n")
        except Exception:
            pass

        time.sleep(0.2)

threading.Thread(target=progress_loop, daemon=True).start()

# cli debug controls for terminal testing
def debug_cli():
    time.sleep(0.5)
    print("\n--- debugers ---")
    print("Commands:")
    print("  p             : Toggle Play/Pause on PC MPRIS")
    print("  n             : Next track on PC MPRIS")
    print("  b             : Previous track on PC MPRIS")
    print("  t <Title - Artist> : Send test track string to ESP32")
    print("  pos <cur> <total>  : Send test progress in seconds (e.g. 'pos 45 210')")
    print("  s <state>     : Send test status to ESP32 ('s Playing' or 's Paused')")
    print("  img <file_or_url>  : Send test image to ESP32 (or 'img clear')")
    print("  reset         : Force MCU back to default Waiting for MPRIS")
    print("  q             : Exit script")
    print("-----------------------\n")

    while True:
        try:
            cmd = input().strip()
            if not cmd:
                continue
            if cmd == 'p':
                send_cum(0)
            elif cmd == 'n':
                send_cum(1)
            elif cmd == 'b':
                send_cum(2)
            elif cmd.startswith('t '):
                custom_txt = cmd[2:].strip()
                if " - " in custom_txt:
                    parts = custom_txt.split(" - ", 1)
                    send_to_mcu(f"TITLE:{clean_mpris_text(parts[0])}\n")
                    send_to_mcu(f"ARTIST:{clean_mpris_text(parts[1])}\n")
                else:
                    send_to_mcu(f"TITLE:{clean_mpris_text(custom_txt)}\n")
                    send_to_mcu("ARTIST:Unknown Artist\n")
            elif cmd.startswith('pos '):
                parts = cmd[4:].strip().split()
                if len(parts) >= 2:
                    send_to_mcu(f"POS:{parts[0]}:{parts[1]}\n")
            elif cmd.startswith('s '):
                custom_status = cmd[2:].strip()
                send_to_mcu(f"STATUS:{custom_status}\n")
            elif cmd.startswith('img '):
                target = cmd[4:].strip()
                if target.lower() == 'clear':
                    send_to_mcu("IMG:CLEAR\n")
                else:
                    data = convert_art_to_rgb565(target)
                    if data:
                        send_image_to_mcu(data)
            elif cmd.startswith('lyric '):
                lyric_txt = cmd[6:].strip()
                send_to_mcu(f"LYRIC:{lyric_txt}\n")
            elif cmd == 'reset':
                send_to_mcu("HOST:DISCONNECTED\n")
            elif cmd == 'q':
                print("stop, exitting.")
                cleanup()
                exit(0)
        except (EOFError, KeyboardInterrupt):
            break

threading.Thread(target=debug_cli, daemon=True).start()

def sync_initial_mpris():
    global had_player
    send_to_mcu("HOST:CONNECTED\n")
    get = get_mpris()
    if not get:
        had_player = False
        print("no active media player found on startup. (play some music or use 't' to simulate')")
        send_to_mcu("TITLE:Waiting for MPRIS...\n")
        send_to_mcu("ARTIST:Launch a music\n")
        send_to_mcu("STATUS:Idle\n")
        send_to_mcu("POS:0:0\n")
        send_to_mcu("IMG:CLEAR\n")
        return
    try:
        had_player = True
        props = dbus.Interface(get, 'org.freedesktop.DBus.Properties')
        metadata = props.Get('org.mpris.MediaPlayer2.Player', 'Metadata')
        status = str(props.Get('org.mpris.MediaPlayer2.Player', 'PlaybackStatus'))

        raw_title = str(metadata.get('xesam:title', 'Waiting for MPRIS...'))
        artists = metadata.get('xesam:artist', ['Unknown Artist'])
        raw_artist = str(artists[0] if (isinstance(artists, (list, tuple)) and len(artists) > 0) else artists)

        titit = clean_mpris_text(raw_title)
        artist = clean_mpris_text(raw_artist)

        pos_us = int(props.Get('org.mpris.MediaPlayer2.Player', 'Position'))
        len_us = int(metadata.get('mpris:length', 0))
        cur_sec = pos_us // 1000000
        total_sec = len_us // 1000000 if len_us > 0 else 0

        send_to_mcu(f"TITLE:{titit}\n")
        send_to_mcu(f"ARTIST:{artist}\n")
        send_to_mcu(f"STATUS:{status}\n")
        send_to_mcu(f"POS:{cur_sec}:{total_sec}\n")

        art_url = str(metadata.get('mpris:artUrl', ''))
        sync_art(art_url)

        track_url = str(metadata.get('xesam:url', ''))
        sync_lyrics_track(titit, artist, total_sec, track_url)

        print(f"synced current track: {titit} - {artist} ({status}) [{cur_sec}/{total_sec}s]")
    except Exception as e:
        print(f"could not sync initial mpris: {e}")

# initial sync
sync_initial_mpris()

def on_properties_changed(interface, changed_properties, invalidated_properties, path=None):
    global had_player
    if 'Metadata' in changed_properties:
        had_player = True
        metadata = changed_properties['Metadata']
        raw_title = str(metadata.get('xesam:title', 'Waiting for MPRIS...'))
        artists = metadata.get('xesam:artist', ['Unknown Artist'])
        raw_artist = str(artists[0] if (isinstance(artists, (list, tuple)) and len(artists) > 0) else artists)

        titit = clean_mpris_text(raw_title)
        artist = clean_mpris_text(raw_artist)

        send_to_mcu(f"TITLE:{titit}\n")
        send_to_mcu(f"ARTIST:{artist}\n")

        len_us = int(metadata.get('mpris:length', 0))
        total_sec = len_us // 1000000 if len_us > 0 else 0
        send_to_mcu(f"POS:0:{total_sec}\n")

        art_url = str(metadata.get('mpris:artUrl', ''))
        sync_art(art_url)

        track_url = str(metadata.get('xesam:url', ''))
        sync_lyrics_track(titit, artist, total_sec, track_url)

    if 'PlaybackStatus' in changed_properties:
        status = str(changed_properties['PlaybackStatus'])
        send_to_mcu(f"STATUS:{status}\n")

ambus.add_signal_receiver(
    on_properties_changed,
    signal_name='PropertiesChanged',
    dbus_interface='org.freedesktop.DBus.Properties',
    path_keyword='path'
)

try:
    gooning = GLib.MainLoop()
    gooning.run()
except KeyboardInterrupt:
    print("stop, exitting.")
    cleanup()

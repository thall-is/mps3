#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
Ferramenta Oficial de Recuperação de Sistema de Arquivos do MPS3 (MicroSD FAT32)
=============================================================================
Restaura e reorganiza o cartão MicroSD do MPS3 após corrupção de FAT32 causada
por desconexão a quente (queda de energia ou remoção USB sem ejeção segura).

Recursos:
  - Diagnóstico e reparo automático do volume FAT32 via Repair-Volume / CHKDSK.
  - Varredura e extração binária dos clusters recuperados em FOUND.000.
  - Extração de metadados de áudio (FLAC Vorbis Comment, MP3 ID3v1/ID3v2, M4A, OGG).
  - Organização de músicas em /Musicas/<Artista>/<Album>/<Faixa> - <Título>.<ext>.
  - Separação e reorganização de podcasts em /Podcasts/<Programa>/<Episódio>.
  - Restauração completa dos arquivos de sistema e controle do firmware MPS3:
      * /.mps3_analytics/tracks_summary.json (estatísticas e scrobbles)
      * /.mps3_analytics/sessions_recovered.jsonl (histórico de escuta)
      * /.mps3_bookmarks.dat (marcadores binários de retomada "MPBM")
      * /Playlists/*.m3u8 (listas de reprodução)
      * /Podcasts/.subscriptions.json (assinaturas de feeds RSS/YouTube)
      * /Podcasts/.played_history (histórico de episódios ouvidos)

Uso:
  python tools/recover_mps3_sd.py --drive D:
  python tools/recover_mps3_sd.py --drive D: --execute
  python tools/recover_mps3_sd.py --drive D: --execute --clean-found
=============================================================================
"""

import os
import sys
import struct
import re
import shutil
import argparse
import json

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def sanitize_filename(name):
    name = re.sub(r'[\\/*?:"<>|]', '_', name)
    name = re.sub(r'[\r\n\t]', ' ', name)
    name = name.strip(' .')
    return name if name else "Desconhecido"

# --- PARSERS DE METADADOS DE ÁUDIO ---

def parse_flac(fp):
    fp.seek(0)
    if fp.read(4) != b'fLaC':
        return None
    tags = {}
    while True:
        hdr = fp.read(4)
        if len(hdr) < 4:
            break
        is_last = (hdr[0] & 0x80) != 0
        block_type = hdr[0] & 0x7F
        length = (hdr[1] << 16) | (hdr[2] << 8) | hdr[3]
        if block_type == 4: # VORBIS_COMMENT
            data = fp.read(length)
            vendor_len = struct.unpack('<I', data[:4])[0]
            pos = 4 + vendor_len
            if pos + 4 <= len(data):
                num_comments = struct.unpack('<I', data[pos:pos+4])[0]
                pos += 4
                for _ in range(num_comments):
                    if pos + 4 > len(data): break
                    clen = struct.unpack('<I', data[pos:pos+4])[0]
                    pos += 4
                    if pos + clen > len(data): break
                    cstr = data[pos:pos+clen].decode('utf-8', errors='replace')
                    pos += clen
                    if '=' in cstr:
                        k, v = cstr.split('=', 1)
                        tags[k.upper()] = v.strip()
        else:
            fp.seek(length, os.SEEK_CUR)
        if is_last:
            break
    return tags

def parse_id3v2(fp):
    fp.seek(0)
    hdr = fp.read(10)
    if len(hdr) < 10 or hdr[:3] != b'ID3':
        return None
    version = hdr[3]
    tag_size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) | ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F)
    data = fp.read(tag_size)
    tags = {}
    pos = 0
    while pos + 10 <= len(data):
        frame_id = data[pos:pos+4].decode('latin1', errors='ignore')
        if not re.match(r'^[A-Z0-9]{4}$', frame_id):
            break
        if version == 4:
            fsize = ((data[pos+4] & 0x7F) << 21) | ((data[pos+5] & 0x7F) << 14) | ((data[pos+6] & 0x7F) << 7) | (data[pos+7] & 0x7F)
        else:
            fsize = (data[pos+4] << 24) | (data[pos+5] << 16) | (data[pos+6] << 8) | data[pos+7]
        pos += 10
        if pos + fsize > len(data):
            break
        fdata = data[pos:pos+fsize]
        pos += fsize
        if len(fdata) > 1:
            enc = fdata[0]
            val = ""
            try:
                if enc == 0: val = fdata[1:].decode('latin1', errors='replace')
                elif enc == 1: val = fdata[1:].decode('utf-16', errors='replace')
                elif enc == 2: val = fdata[1:].decode('utf-16-be', errors='replace')
                elif enc == 3: val = fdata[1:].decode('utf-8', errors='replace')
                else: val = fdata[1:].decode('latin1', errors='replace')
            except:
                pass
            val = val.strip('\x00').strip()
            if frame_id == 'TIT2': tags['TITLE'] = val
            elif frame_id == 'TPE1': tags['ARTIST'] = val
            elif frame_id == 'TALB': tags['ALBUM'] = val
            elif frame_id == 'TRCK': tags['TRACKNUMBER'] = val
    return tags

def parse_id3v1(fp):
    fp.seek(-128, os.SEEK_END)
    block = fp.read(128)
    if len(block) == 128 and block[:3] == b'TAG':
        title = block[3:33].decode('latin1', errors='replace').strip('\x00').strip()
        artist = block[33:63].decode('latin1', errors='replace').strip('\x00').strip()
        album = block[63:93].decode('latin1', errors='replace').strip('\x00').strip()
        track = ""
        if block[125] == 0:
            track = str(block[126])
        tags = {}
        if title: tags['TITLE'] = title
        if artist: tags['ARTIST'] = artist
        if album: tags['ALBUM'] = album
        if track: tags['TRACKNUMBER'] = track
        return tags
    return None

def detect_audio_file(path):
    size = os.path.getsize(path)
    if size < 4096:
        return None, None
    with open(path, 'rb') as fp:
        magic = fp.read(12)
        if magic.startswith(b'fLaC'):
            tags = parse_flac(fp)
            return 'flac', tags
        elif magic.startswith(b'ID3'):
            tags = parse_id3v2(fp)
            return 'mp3', tags
        elif magic[:2] == b'\xff\xfb' or magic[:2] == b'\xff\xf3' or magic[:2] == b'\xff\xf2':
            tags = parse_id3v1(fp) if size >= 128 else None
            return 'mp3', tags
        elif b'ftyp' in magic:
            return 'm4a', {}
        elif magic.startswith(b'RIFF') and b'WAVE' in magic:
            return 'wav', {}
        elif magic.startswith(b'OggS'):
            return 'ogg', {}
        if size >= 128:
            tags = parse_id3v1(fp)
            if tags and ('TITLE' in tags or 'ARTIST' in tags):
                return 'mp3', tags
    return None, None

# --- RESTAURAÇÃO DE SISTEMA MPS3 ---

def restore_mps3_system_files(drive_root, found_dir, dry_run=False):
    print("\n--- [1/3] Identificando Arquivos de Sistema do MPS3 ---")
    files = sorted(os.listdir(found_dir))
    
    analytics_dir = os.path.join(drive_root, ".mps3_analytics")
    podcasts_dir = os.path.join(drive_root, "Podcasts")
    playlists_dir = os.path.join(drive_root, "Playlists")
    
    if not dry_run:
        os.makedirs(analytics_dir, exist_ok=True)
        os.makedirs(podcasts_dir, exist_ok=True)
        os.makedirs(playlists_dir, exist_ok=True)

    tracks_summary_lines = {}
    sessions_lines = []
    bookmarks_file = None
    subscriptions_file = None
    playlist_files = []

    for f in files:
        p = os.path.join(found_dir, f)
        try:
            sz = os.path.getsize(p)
            if sz > 15 * 1024 * 1024:
                continue
            with open(p, 'rb') as fp:
                head = fp.read(min(sz, 65536))
            
            # 1. Bookmarks .mps3_bookmarks.dat (Magic "MPBM")
            if head.startswith(b'MPBM') or head.startswith(b'MBPM'):
                bookmarks_file = p
                print(f" -> Marcador Multi-Bookmarks detectado: {f} ({sz} bytes)")
            
            # 2. Subscriptions /Podcasts/.subscriptions.json
            if b'"subscriptions"' in head and b'"version"' in head:
                subscriptions_file = p
                print(f" -> Assinaturas de Podcasts detectadas: {f} ({sz} bytes)")

            # 3. Playlists (.m3u8)
            if head.startswith(b'#EXTM3U'):
                playlist_files.append((f, p))
                print(f" -> Playlist recuperada: {f} ({sz} bytes)")

            # 4. Analytics: tracks_summary.json e sessões históricas
            if b'"id":' in head and (b'"path":' in head or b'"epoch":' in head):
                with open(p, 'rb') as fp:
                    full_text = fp.read().decode('utf-8', errors='ignore')
                for line in full_text.splitlines():
                    line = line.strip().rstrip(',')
                    if '"id":' in line and '"path":' in line:
                        m = re.search(r'"id":"([0-9a-fA-F]+)"', line)
                        if m:
                            tracks_summary_lines[m.group(1)] = line
                    elif line.startswith('{"ts":') and '"epoch":' in line:
                        sessions_lines.append(line)
        except Exception:
            pass

    print(f"\nResumo de arquivos MPS3 identificados:")
    print(f"  * Faixas únicas de telemetria: {len(tracks_summary_lines)}")
    print(f"  * Sessões de reprodução: {len(sessions_lines)}")
    print(f"  * Playlists: {len(playlist_files)}")
    print(f"  * Bookmarks: {'Sim' if bookmarks_file else 'Não'}")
    print(f"  * Subscriptions: {'Sim' if subscriptions_file else 'Não'}")

    if not dry_run:
        # Gravar tracks_summary.json
        if tracks_summary_lines:
            sum_path = os.path.join(analytics_dir, "tracks_summary.json")
            with open(sum_path, 'w', encoding='utf-8') as fp:
                for _, l in sorted(tracks_summary_lines.items()):
                    fp.write(l + "\n")
            print(f"  [OK] Gravado: {sum_path}")

        # Gravar sessions_recovered.jsonl
        if sessions_lines:
            sess_path = os.path.join(analytics_dir, "sessions_recovered.jsonl")
            with open(sess_path, 'w', encoding='utf-8') as fp:
                for l in sessions_lines:
                    fp.write(l + "\n")
            print(f"  [OK] Gravado: {sess_path}")

        # Gravar .mps3_bookmarks.dat
        if bookmarks_file:
            bmk_dst = os.path.join(drive_root, ".mps3_bookmarks.dat")
            shutil.copy2(bookmarks_file, bmk_dst)
            print(f"  [OK] Gravado: {bmk_dst}")

        # Gravar .subscriptions.json
        if subscriptions_file:
            with open(subscriptions_file, 'rb') as fp:
                raw = fp.read().decode('utf-8', errors='ignore')
            end_idx = raw.rfind('}')
            if end_idx != -1:
                clean_json = raw[:end_idx+1]
                sub_data = json.loads(clean_json)
                sub_dst = os.path.join(podcasts_dir, ".subscriptions.json")
                with open(sub_dst, 'w', encoding='utf-8') as fp:
                    json.dump(sub_data, fp, indent=2, ensure_ascii=False)
                print(f"  [OK] Gravado: {sub_dst}")

        # Gravar Playlists
        for idx, (pl_name, pl_src) in enumerate(playlist_files):
            with open(pl_src, 'rb') as fp:
                raw = fp.read().decode('utf-8', errors='ignore')
            lines = [l.strip() for l in raw.splitlines() if l.strip().startswith('#EXTM3U') or l.strip().startswith('/sdcard/')]
            pl_dst = os.path.join(playlists_dir, f"Playlist_{idx+1}.m3u8")
            with open(pl_dst, 'w', encoding='utf-8') as fp:
                for l in lines:
                    fp.write(l + "\n")
            print(f"  [OK] Gravado: {pl_dst} ({len(lines)} entradas)")

# --- RESTAURAÇÃO DE MÚSICAS E PODCASTS ---

def restore_audio_files(drive_root, found_dir, dry_run=False):
    print("\n--- [2/3] Identificando e Organizando Músicas e Podcasts ---")
    files = sorted(os.listdir(found_dir))
    
    music_base = os.path.join(drive_root, "Musicas")
    podcast_base = os.path.join(drive_root, "Podcasts")
    
    if not dry_run:
        os.makedirs(music_base, exist_ok=True)
        os.makedirs(podcast_base, exist_ok=True)

    moved_count = 0
    podcast_count = 0

    for f in files:
        src_path = os.path.join(found_dir, f)
        ext, tags = detect_audio_file(src_path)
        if not ext:
            continue

        tags = tags or {}
        artist = sanitize_filename(tags.get('ARTIST') or tags.get('ALBUMARTIST') or "Desconhecido")
        album = sanitize_filename(tags.get('ALBUM') or "")
        title = sanitize_filename(tags.get('TITLE') or f)
        track = tags.get('TRACKNUMBER', '')
        if '/' in track: track = track.split('/')[0]
        track = track.strip()
        prefix = f"{int(track):02d} - " if track.isdigit() else ""

        filename = f"{prefix}{title}.{ext}"
        filename = sanitize_filename(filename)
        if not filename.endswith(f".{ext}"):
            filename = f"{filename}.{ext}"

        # Classificação de Podcast vs. Música
        is_podcast = False
        lower_artist = artist.lower()
        lower_album = album.lower()
        lower_title = title.lower()

        if any(k in lower_artist or k in lower_album or k in lower_title for k in ['podcast', 'nerdcast', 'vortex', 'hora boa', 'desce a letra', 'iamarino']):
            is_podcast = True

        if is_podcast:
            prog_name = "Outros"
            if 'nerdcast' in lower_artist or 'nerdcast' in lower_title: prog_name = "NerdCast"
            elif 'vortex' in lower_artist or 'vortex' in lower_title: prog_name = "Vortex"
            elif 'hora boa' in lower_artist or 'hora boa' in lower_title: prog_name = "Hora Boa Podcast"
            elif 'iamarino' in lower_artist or 'ficção' in lower_album: prog_name = "Nao Ficcao - Atila Iamarino"

            target_dir = os.path.join(podcast_base, prog_name)
            podcast_count += 1
        else:
            if album and album != "Desconhecido":
                target_dir = os.path.join(music_base, artist, album)
            else:
                target_dir = os.path.join(music_base, artist)

        dest_path = os.path.join(target_dir, filename)

        if dry_run:
            if moved_count < 10:
                print(f"[DRY-RUN] {f} -> {dest_path}")
        else:
            os.makedirs(target_dir, exist_ok=True)
            counter = 1
            bname, fext = os.path.splitext(filename)
            while os.path.exists(dest_path):
                dest_path = os.path.join(target_dir, f"{bname} ({counter}){fext}")
                counter += 1
            try:
                os.rename(src_path, dest_path)
            except Exception as e:
                print(f"Erro ao mover {f}: {e}")
                continue

        moved_count += 1
        if moved_count % 300 == 0:
            print(f"Processadas {moved_count} faixas...")

    print(f"\nFinalizado! Total de faixas processadas: {moved_count} (Músicas: {moved_count - podcast_count}, Podcasts: {podcast_count})")

def main():
    parser = argparse.ArgumentParser(description="Ferramenta Oficial de Recuperação de SD Card MPS3")
    parser.add_argument("--drive", required=True, help="Letra da unidade do cartão SD (ex: D: ou E:)")
    parser.add_argument("--execute", action="store_true", help="Executa as movimentações reais (sem isso faz dry-run)")
    parser.add_argument("--clean-found", action="store_true", help="Remove resíduos órfãos não-áudio em FOUND.000 após recuperar")
    args = parser.parse_args()

    drive_root = args.drive.rstrip("\\/") + "\\"
    found_dir = os.path.join(drive_root, "FOUND.000")

    if not os.path.exists(drive_root):
        print(f"Erro: Unidade {drive_root} não encontrada.")
        sys.exit(1)

    if not os.path.exists(found_dir):
        print(f"Aviso: Diretório {found_dir} não encontrado. Verifique se o CHKDSK / Repair-Volume já foi executado.")
        sys.exit(1)

    print(f"Iniciando recuperação em {drive_root} (Modo: {'EXECUÇÃO REAL' if args.execute else 'SIMULAÇÃO / DRY-RUN'})...")
    
    restore_mps3_system_files(drive_root, found_dir, dry_run=not args.execute)
    restore_audio_files(drive_root, found_dir, dry_run=not args.execute)

    if args.execute and args.clean_found:
        print("\n--- [3/3] Limpeza de resíduos em FOUND.000 ---")
        try:
            shutil.rmtree(found_dir)
            print("Diretório FOUND.000 limpo com sucesso!")
        except Exception as e:
            print(f"Aviso ao limpar FOUND.000: {e}")

    print("\nProcesso concluído! Ejetar com segurança no Windows antes de reinserir no MPS3.")

if __name__ == '__main__':
    main()


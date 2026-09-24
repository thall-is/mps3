#!/usr/bin/env python3
import gzip
import os
import sys

def pack_web():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, ".."))
    html_path = os.path.join(project_root, "components", "wifi_transfer", "web", "index.html")
    header_path = os.path.join(project_root, "components", "wifi_transfer", "include", "index_html_gz.h")

    if not os.path.exists(html_path):
        print(f"Erro: {html_path} nao encontrado!")
        sys.exit(1)

    with open(html_path, "rb") as f:
        raw_data = f.read()

    gz_data = gzip.compress(raw_data, compresslevel=9)

    print(f"Tamanho original: {len(raw_data)} bytes")
    print(f"Tamanho gzip:     {len(gz_data)} bytes ({len(gz_data)/len(raw_data)*100:.1f}%)")

    lines = []
    lines.append("// Gerado automaticamente com gzip compactado")
    lines.append("#pragma once")
    lines.append("")
    lines.append(f"const size_t index_html_gz_size = {len(gz_data)};")
    lines.append("const uint8_t index_html_gz[] = {")

    chunk_size = 16
    for i in range(0, len(gz_data), chunk_size):
        chunk = gz_data[i:i+chunk_size]
        hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + chunk_size < len(gz_data):
            lines.append(f"    {hex_bytes},")
        else:
            lines.append(f"    {hex_bytes}")

    lines.append("};")
    lines.append("")

    with open(header_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"Header C gerado em: {header_path}")

if __name__ == "__main__":
    pack_web()

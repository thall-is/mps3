#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MPS3 Web UI Modular Assembler & Gzip Packager (v2.0)
----------------------------------------------------
1. Lê `components/wifi_transfer/web/src/index.template.html` e expande todas as
   diretivas `<!-- @include caminho/relativo -->` a partir de `src/`.
2. Valida integridade UTF-8, ausência de Mojibake e unicidade de IDs HTML nas views.
3. Gera o bundle legível `components/wifi_transfer/web/index.html`.
4. Compacta com Gzip nível 9 determinístico (mtime=0) e gera o header C
   `components/wifi_transfer/include/index_html_gz.h`.
"""

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
import re
import sys

INCLUDE_PATTERN = re.compile(r"^\s*<!--\s*@include\s+([^\s]+)\s*-->\s*$")
MOJIBAKE_TOKENS = ("Ã£", "Ã§", "Ã©", "Ãª", "Ã³", "Ãº", "Ã¡", "Ã\xad", "\ufffd")


def get_paths(project_root=None):
    if project_root is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.abspath(os.path.join(script_dir, ".."))
    web_dir = os.path.join(project_root, "components", "wifi_transfer", "web")
    src_dir = os.path.join(web_dir, "src")
    template_path = os.path.join(src_dir, "index.template.html")
    html_path = os.path.join(web_dir, "index.html")
    header_path = os.path.join(project_root, "components", "wifi_transfer", "include", "index_html_gz.h")
    return {
        "project_root": project_root,
        "web_dir": web_dir,
        "src_dir": src_dir,
        "template_path": template_path,
        "html_path": html_path,
        "header_path": header_path,
    }


def check_utf8_and_mojibake(file_path, text):
    for token in MOJIBAKE_TOKENS:
        if token in text:
            raise ValueError(f"Possível corrupção UTF-8 (Mojibake '{token}') detectada em: {file_path}")


def validate_unique_html_ids(src_dir):
    views_dir = os.path.join(src_dir, "views")
    if not os.path.isdir(views_dir):
        return []
    id_map = {}
    duplicates = []
    # Captura id="foo" ou id='foo' estáticos (ignora templates JS `${...}`)
    id_regex = re.compile(r"""\bid\s*=\s*["']([^"'${}]+)["']""")
    for fname in sorted(os.listdir(views_dir)):
        if not fname.endswith(".html"):
            continue
        fpath = os.path.join(views_dir, fname)
        with open(fpath, "r", encoding="utf-8") as f:
            content = f.read()
        for match in id_regex.finditer(content):
            el_id = match.group(1).strip()
            if not el_id:
                continue
            if el_id in id_map:
                duplicates.append((el_id, id_map[el_id], fname))
            else:
                id_map[el_id] = fname
    return duplicates


def assemble_from_src(src_dir, template_path):
    if not os.path.exists(template_path):
        raise FileNotFoundError(f"Template mestre não encontrado: {template_path}")

    with open(template_path, "r", encoding="utf-8") as f:
        template_lines = f.readlines()

    check_utf8_and_mojibake(template_path, "".join(template_lines))

    duplicates = validate_unique_html_ids(src_dir)
    if duplicates:
        dup_msgs = [f"id='{d[0]}' duplicado entre {d[1]} e {d[2]}" for d in duplicates]
        raise ValueError("IDs HTML duplicados nas views:\n  - " + "\n  - ".join(dup_msgs))

    assembled_lines = []
    included_files = []

    for idx, line in enumerate(template_lines):
        if idx == 0 and line.lower().startswith("<!doctype html>"):
            assembled_lines.append(line.rstrip("\r\n") + "\n")
            assembled_lines.append(
                "<!-- ARQUIVO GERADO AUTOMATICAMENTE POR tools/pack_web.py A PARTIR DE components/wifi_transfer/web/src/ - NAO EDITE DIRETAMENTE -->\n"
            )
            continue

        m = INCLUDE_PATTERN.match(line)
        if m:
            rel_inc = m.group(1).strip().replace("\\", "/")
            inc_path = os.path.normpath(os.path.join(src_dir, rel_inc))
            if not os.path.exists(inc_path):
                raise FileNotFoundError(
                    f"Módulo incluído '{rel_inc}' não encontrado (esperado em {inc_path})"
                )
            with open(inc_path, "r", encoding="utf-8") as inc_f:
                inc_text = inc_f.read()
            check_utf8_and_mojibake(inc_path, inc_text)
            included_files.append(rel_inc)
            assembled_lines.append(inc_text.rstrip("\r\n") + "\n")
        else:
            assembled_lines.append(line.rstrip("\r\n") + "\n")

    full_html = "".join(assembled_lines)
    if "@include" in full_html:
        raise ValueError("Diretiva @include residual encontrada após montagem!")

    if "__MPS3_WEB_BUILD_HASH__" in full_html:
        build_hash = hashlib.sha256(full_html.encode("utf-8")).hexdigest()[:12]
        full_html = full_html.replace("__MPS3_WEB_BUILD_HASH__", build_hash)

    return full_html, included_files


def format_c_header(gz_data):
    lines = [
        "// Gerado automaticamente com gzip compactado (tools/pack_web.py v2.0)",
        "#pragma once",
        "",
        f"const size_t index_html_gz_size = {len(gz_data)};",
        "const uint8_t index_html_gz[] = {",
    ]
    chunk_size = 16
    for i in range(0, len(gz_data), chunk_size):
        chunk = gz_data[i : i + chunk_size]
        hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + chunk_size < len(gz_data):
            lines.append(f"    {hex_bytes},")
        else:
            lines.append(f"    {hex_bytes}")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def pack_web(check_only=False, export_cloud_dir=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    paths = get_paths()
    src_dir = paths["src_dir"]
    template_path = paths["template_path"]
    html_path = paths["html_path"]
    header_path = paths["header_path"]

    if os.path.exists(template_path):
        assembled_html, included_files = assemble_from_src(src_dir, template_path)
        raw_data = assembled_html.encode("utf-8")
        if not check_only:
            with open(html_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(assembled_html)
            print(f"[Modular Build] {len(included_files)} módulos montados -> {html_path}")
    else:
        if not os.path.exists(html_path):
            print(f"Erro: {html_path} nao encontrado!")
            sys.exit(1)
        with open(html_path, "rb") as f:
            raw_data = f.read()

    gz_data = gzip.compress(raw_data, compresslevel=9, mtime=0)
    full_sha256 = hashlib.sha256(raw_data).hexdigest()
    m_hash = re.search(r'MPS3_WEB_BUILD_HASH\s*=\s*"([0-9a-f]{12})"', raw_data.decode("utf-8", errors="ignore"))
    short_hash = m_hash.group(1) if m_hash else full_sha256[:12]

    print(f"Build Hash:       #{short_hash}")
    print(f"Tamanho original: {len(raw_data)} bytes ({len(raw_data)/1024:.1f} KB)")
    print(f"Tamanho gzip:     {len(gz_data)} bytes ({len(gz_data)/len(raw_data)*100:.1f}%)")

    if export_cloud_dir:
        os.makedirs(export_cloud_dir, exist_ok=True)
        cloud_html_path = os.path.join(export_cloud_dir, "index.html")
        cloud_ver_path = os.path.join(export_cloud_dir, "version.json")
        cloud_nojekyll = os.path.join(export_cloud_dir, ".nojekyll")
        with open(cloud_html_path, "wb") as f:
            f.write(raw_data)
        manifest = {
            "hash": short_hash,
            "sha256": full_sha256,
            "size": len(raw_data),
            "gzip_size": len(gz_data),
            "built_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        with open(cloud_ver_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        with open(cloud_nojekyll, "w", encoding="utf-8") as f:
            f.write("")
        print(f"[Cloud Export] Artefatos GitHub Pages gerados em: {os.path.abspath(export_cloud_dir)}")

    if check_only:
        return 0

    header_content = format_c_header(gz_data)
    with open(header_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header_content)

    print(f"Header C gerado em: {header_path}")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MPS3 Web UI Modular Assembler & Packager")
    parser.add_argument("--check", action="store_true", help="Apenas valida os módulos sem sobrescrever arquivos")
    parser.add_argument("--export-cloud", metavar="DIR", help="Exporta index.html, version.json e .nojekyll para deploy no GitHub Pages")
    args = parser.parse_args()
    sys.exit(pack_web(check_only=args.check, export_cloud_dir=args.export_cloud))

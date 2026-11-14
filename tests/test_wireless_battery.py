import urllib.request
import urllib.error
import json
import hashlib
import os
import sys
import time

TARGET = os.environ.get('MPS3_TARGET', 'http://mps3.local')

def req(url, data=None, headers=None, method=None):
    h = headers or {}
    r = urllib.request.Request(url, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r, timeout=10) as resp:
            return resp.status, resp.read(), dict(resp.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)

def run():
    print('=== BATERIA DE TESTES SEM FIOS MPS3 v2.0 ===\n')

    # [1] Status & OTA Info
    s, b, _ = req(f'{TARGET}/api/ota')
    ota = json.loads(b)
    print(f'[1] Device OTA & Bateria:')
    print(f'    Status: {s} | MAC: {ota.get("mac")} | Bateria: {ota.get("battery_percent")}% (Charging: {ota.get("battery_charging")})')
    assert s == 200

    # [2] GET /api/sync/manifest
    s, b, _ = req(f'{TARGET}/api/sync/manifest')
    m = json.loads(b)
    free_mb = m.get('free_bytes', 0) / (1024 * 1024)
    print(f'\n[2] GET /api/sync/manifest:')
    print(f'    Status: {s} | Espaco Livre: {free_mb:.1f} MB | Podcasts no SD: {len(m.get("podcasts", []))} | Historico de Ouvidos: {len(m.get("played_history", []))}')
    assert s == 200
    assert 'device_mac' in m
    assert 'free_bytes' in m

    # [3] POST /api/sync/begin
    s, b, _ = req(f'{TARGET}/api/sync/begin', data=b'{}', headers={'Content-Type':'application/json'}, method='POST')
    print(f'\n[3] POST /api/sync/begin:')
    print(f'    Status: {s} | Resposta: {b.decode().strip()}')
    assert s == 200

    # [4] Double SHA-256 Engine: Upload com Hash Invalido (Esperado 422 Unprocessable Entity)
    payload = os.urandom(64 * 1024)
    real_sha = hashlib.sha256(payload).hexdigest()
    fake_sha = '0' * 64

    s, b, _ = req(f'{TARGET}/api/upload?path=%2FPodcasts%2F.staging%2Ftest_sha_bad.bin',
                  data=payload,
                  headers={'Content-Type':'application/octet-stream', 'X-Expected-SHA256': fake_sha},
                  method='PUT')
    print(f'\n[4] Upload com SHA-256 FALSO (deve rejeitar com 422):')
    print(f'    Status: {s} | Resposta: {b.decode().strip()}')
    assert s == 422
    assert 'sha256_mismatch' in b.decode()

    # [5] Confirmacao de que o arquivo rejeitado no 422 foi removido imediatamente
    s, b, _ = req(f'{TARGET}/api/sync/verify',
                  data=json.dumps({'path': 'Podcasts/.staging/test_sha_bad.bin'}).encode(),
                  headers={'Content-Type':'application/json'},
                  method='POST')
    print(f'\n[5] Verificacao de remocao automatica do arquivo corrompido:')
    print(f'    Status: {s} (Esperado 404 Not Found) | Resposta: {b.decode().strip()}')
    assert s == 404

    # [6] Upload com SHA-256 Valido (Esperado 200 OK + hash retornado)
    t0 = time.time()
    s, b, h = req(f'{TARGET}/api/upload?path=%2FPodcasts%2F.staging%2Ftest_sha_good.bin',
                  data=payload,
                  headers={'Content-Type':'application/octet-stream', 'X-Expected-SHA256': real_sha},
                  method='PUT')
    dt = time.time() - t0
    resp_json = json.loads(b)
    speed_mbs = (len(payload) / (1024 * 1024)) / max(dt, 0.001)
    print(f'\n[6] Upload com SHA-256 CORRETO:')
    print(f'    Status: {s} | Tempo: {dt:.2f}s ({speed_mbs:.2f} MB/s)')
    print(f'    SHA-256 Retornado: {resp_json.get("sha256")}')
    print(f'    SHA-256 Esperado:  {real_sha}')
    assert s == 200
    assert resp_json.get('sha256') == real_sha

    # [7] POST /api/sync/verify (Leitura fisica do cartao SD conferindo o hash)
    s, b, _ = req(f'{TARGET}/api/sync/verify',
                  data=json.dumps({'path': 'Podcasts/.staging/test_sha_good.bin'}).encode(),
                  headers={'Content-Type':'application/json'},
                  method='POST')
    v = json.loads(b)
    print(f'\n[7] POST /api/sync/verify (Read-back nos setores fisicos do SD card):')
    print(f'    Status: {s} | Tamanho: {v.get("size")} B')
    print(f'    SHA-256 no SD: {v.get("sha256")}')
    print(f'    Hash Confere:  {v.get("sha256") == real_sha}')
    assert s == 200
    assert v.get('sha256') == real_sha

    # [8] POST /api/rename (Mover atomicamente de .staging para pasta final)
    s, b, _ = req(f'{TARGET}/api/rename',
                  data=json.dumps({'old_path': 'Podcasts/.staging/test_sha_good.bin', 'new_path': 'Podcasts/TestShow/test_sha_good.bin'}).encode(),
                  headers={'Content-Type':'application/json'},
                  method='POST')
    print(f'\n[8] POST /api/rename (.staging/ -> final):')
    print(f'    Status: {s} | Resposta: {b.decode().strip()}')
    assert s == 200

    # [9] POST /api/podcasts/played (Marcacao e desmarcacao de status ouvido)
    s, b, _ = req(f'{TARGET}/api/podcasts/played',
                  data=json.dumps({'rel_path': 'TestShow/test_sha_good.bin', 'played': True}).encode(),
                  headers={'Content-Type':'application/json'},
                  method='POST')
    print(f'\n[9] POST /api/podcasts/played:')
    print(f'    Marcar como ouvido: Status={s}')
    assert s == 200

    s, b, _ = req(f'{TARGET}/api/sync/manifest')
    m = json.loads(b)
    found = [p for p in m.get('podcasts', []) if 'test_sha_good' in p.get('filename', '')]
    is_played = found[0].get('played') if found else None
    print(f'    Verificacao no manifesto: Encontrado={len(found) > 0} | Played={is_played}')
    assert is_played == True

    # Desmarcar
    s, b, _ = req(f'{TARGET}/api/podcasts/played',
                  data=json.dumps({'rel_path': 'TestShow/test_sha_good.bin', 'played': False}).encode(),
                  headers={'Content-Type':'application/json'},
                  method='POST')
    print(f'    Desmarcar como ouvido: Status={s}')
    assert s == 200

    # [10] Limpeza do arquivo de teste
    s, b, _ = req(f'{TARGET}/api/delete?path=%2FPodcasts%2FTestShow%2Ftest_sha_good.bin', method='DELETE')
    print(f'\n[10] DELETE /api/delete (Limpeza do arquivo de teste):')
    print(f'    Status: {s}')
    assert s == 200

    # [11] POST /api/sync/done
    s, b, _ = req(f'{TARGET}/api/sync/done', data=b'{}', headers={'Content-Type':'application/json'}, method='POST')
    print(f'\n[11] POST /api/sync/done (Conclusao da sessao de sync):')
    print(f'    Status: {s} | Resposta: {b.decode().strip()}')
    assert s == 200

    print('\n=================================================================')
    print('[OK] RESULTADO: TODOS OS 11 TESTES SEM FIOS PASSARAM COM 100% DE SUCESSO!')
    print('=================================================================')

if __name__ == '__main__':
    run()

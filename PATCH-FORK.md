# Patch Fork - Metrics Logging per llama-server

**Data**: 22 marzo 2026  
**Versione llama.cpp**: commit 49bfddeca  
**Autore**: Patch personalizzata per tracking metriche

---

## 📋 Panoramica

Questa è una patch **personale** che aggiunge il logging delle metriche di consumo token per `llama-server` in formato JSON Lines.

**NON è destinata al merge nella repo principale** - è un fork personale per uso interno.

---

## 🎯 Cosa fa

Aggiunge un nuovo flag `--metrics-file` che registra ogni richiesta completata con:

- **Token consumption**: input, output, total
- **Performance**: throughput, TTFT, tempo per token
- **Prompt cache**: hit/miss, token riutilizzati, risparmi cumulativi
- **KV cache**: utilizzo, token in cache
- **Server state**: slot occupati, queue depth

---

## 📦 File Modificati

```
common/arg.cpp                  |   7 +++
common/common.h                 |   3 ++
tools/server/server-context.cpp | 187 ++++++++++++++++++++++++++++++++++-
3 files changed, 196 insertions(+), 1 deletion(-)
```

### Dettaglio modifiche:

1. **common/common.h**
   - Aggiunto campo `std::string metrics_file` a `common_params`

2. **common/arg.cpp**
   - Aggiunto flag `--metrics-file PATH` con env var `LLAMA_METRICS_FILE`

3. **tools/server/server-context.cpp**
   - Nuova struct `server_metrics_writer` per logging JSON Lines
   - Estesa `server_metrics` con tracking cache hit/miss
   - Nuovo campo `n_prompt_tokens_cached` in `server_slot`
   - Logging ad ogni richiesta completata

---

## 🚀 Utilizzo

### Compilazione
```bash
cd llama.cpp
cmake -B build
cmake --build build --target llama-server -j8
```

### Esecuzione
```bash
# Con flag CLI
./build/bin/llama-server -m modello.gguf --metrics-file metrics.jsonl

# Con variabile d'ambiente
LLAMA_METRICS_FILE=metrics.jsonl ./build/bin/llama-server -m modello.gguf
```

---

## 📊 Formato Output

Ogni riga è un oggetto JSON autonomo (NDJSON):

```json
{"timestamp":1742687123,"request_id":"req_1","input_tokens":50,"output_tokens":100,"total_tokens":150,"tokens_per_second_total":150.5,"input_tokens_per_second":200.0,"output_tokens_per_second":125.3,"time_to_first_token_ms":250.0,"time_per_token_ms":8.50,"time_total_ms":750.0,"prompt_cache_hit":true,"prompt_cache_hit_rate_cumulative":0.850,"prompt_cache_tokens_reused":50,"prompt_cache_savings_tokens_total":200,"kv_cache_usage_percent":75.5,"kv_cache_tokens":1024,"queue_depth":0,"slots_busy":1,"slots_total":4,"stop_reason":"eos","generation_length_ratio":0.75}
```

### Campi disponibili:

| Campo | Tipo | Descrizione |
|-------|------|-------------|
| `timestamp` | int64 | Unix epoch (secondi) |
| `request_id` | string | Identificativo richiesta (req_N) |
| `input_tokens` | int | Token di input |
| `output_tokens` | int | Token generati |
| `total_tokens` | int | Input + Output |
| `tokens_per_second_total` | float | Throughput complessivo |
| `input_tokens_per_second` | float | Velocità input |
| `output_tokens_per_second` | float | Velocità output |
| `time_to_first_token_ms` | float | Latenza iniziale (TTFT) |
| `time_per_token_ms` | float | Tempo medio per token |
| `time_total_ms` | float | Durata totale |
| `prompt_cache_hit` | bool | Cache hit? |
| `prompt_cache_hit_rate_cumulative` | float | % hit cumulativa (0-1) |
| `prompt_cache_tokens_reused` | int | Token riutilizzati |
| `prompt_cache_savings_tokens_total` | int64 | Token risparmiati (totale) |
| `kv_cache_usage_percent` | float | % utilizzo KV cache |
| `kv_cache_tokens` | int | Token in KV cache |
| `queue_depth` | int | Richieste in coda |
| `slots_busy` | int | Slot occupati |
| `slots_total` | int | Slot totali |
| `stop_reason` | string | "eos", "stop_word", "length" |
| `generation_length_ratio` | float | Ratio generati/max |

---

## 📈 Analisi Dati

### Con jq (bash)
```bash
# Totale token processati
jq -s '[.[].total_tokens] | add' metrics.jsonl

# Token input/output separati
jq -s '[.[].input_tokens] | add' metrics.jsonl
jq -s '[.[].output_tokens] | add' metrics.jsonl

# Cache hit rate %
jq -s '[-1].prompt_cache_hit_rate_cumulative * 100' metrics.jsonl

# Token risparmiati
jq -s '[-1].prompt_cache_savings_tokens_total' metrics.jsonl

# Throughput medio
jq -s '[.[].tokens_per_second_total] | add / length' metrics.jsonl

# TTFT medio
jq -s '[.[].time_to_first_token_ms] | add / length' metrics.jsonl

# Esporta per grafici
jq -r '[.timestamp, .tokens_per_second_total] | @csv' metrics.jsonl > throughput.csv
```

### Con Python
```python
import json

metrics = [json.loads(line) for line in open('metrics.jsonl')]

total_tokens = sum(m['total_tokens'] for m in metrics)
cache_hit_rate = metrics[-1]['prompt_cache_hit_rate_cumulative'] * 100
tokens_saved = metrics[-1]['prompt_cache_savings_tokens_total']

print(f"Token totali: {total_tokens}")
print(f"Cache hit rate: {cache_hit_rate:.1f}%")
print(f"Token risparmiati: {tokens_saved}")
```

---

## 🔧 Applicare/Rimuovere la Patch

### Applicare
```bash
cd llama.cpp
git apply ~/Downloads/llama-server-metrics-patch.diff
```

### Rimuovere
```bash
git apply -R ~/Downloads/llama-server-metrics-patch.diff
```

### Verificare stato
```bash
git status
git diff --stat
```

---

## 📁 File Correlati

| File | Descrizione |
|------|-------------|
| `~/Downloads/llama-server-metrics-patch.diff` | Patch git completa |
| `~/Downloads/lama-metriche.jsonl` | Esempio output metriche |
| `METRICS_PATCH_README.md` | Documentazione estesa |
| `PATCH-FORK.md` | Questo file |

---

## ⚠️ Note Importanti

### Git Workflow
- **NON fare push su origin/master** - questa è una patch personale
- La patch è mantenuta in `~/Downloads/llama-server-metrics-patch.diff`
- Per aggiornare: rigenerare la patch con `git diff > ~/Downloads/llama-server-metrics-patch.diff`

### Limitazioni Conosciute
- `queue_depth` è sempre 0 (la queue interna non è accessibile)
- `generation_length_ratio` è 0 se `n_predict` non è impostato
- Memory usage non è incluso (stima inaccurata)

### Performance
- Overhead minimo: scrittura append-only ad ogni richiesta
- File JSON Lines: sicuro, se il server crasha non perdi dati precedenti

---

## 🔄 Aggiornamenti Futuri

Se la repo upstream viene aggiornata:

1. Fetch nuovi cambiamenti:
   ```bash
   git fetch origin
   git rebase origin/master
   ```

2. Risolvi eventuali conflitti su:
   - `common/arg.cpp`
   - `common/common.h`
   - `tools/server/server-context.cpp`

3. Rigenera la patch:
   ```bash
   git diff > ~/Downloads/llama-server-metrics-patch.diff
   ```

---

## 📝 Changelog

### v1.0 - 22 marzo 2026
- Implementazione iniziale
- Flag `--metrics-file` con supporto env var
- Logging JSON Lines con tutte le metriche principali
- Tracking corretto prompt cache hit/miss
- Timestamp Unix epoch per grafici temporali

---

## 📞 Supporto

Per domande o problemi con questa patch personale, contattare internamente.

**Questa patch NON è supportata ufficialmente da llama.cpp**

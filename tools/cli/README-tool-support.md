# llama-cli Tool Call Support

Implementazione modulare di tool-call per llama-cli, ottimizzata per programmazione (Swift 6 focus).

## Architettura

```
tools/cli/
├── cli.cpp              # Main loop integrato con tool support
├── cli-tool.h           # Strutture dati tool
├── cli-tool.cpp         # Tool registry e definizioni
├── cli-tool-exec.h      # Interfaccia esecuzione tool
├── cli-tool-exec.cpp    # Implementazione esecuzione + sicurezza
├── cli-tool-parser.h    # Parser tool call
├── cli-tool-parser.cpp  # Implementazione parser
├── cli-stats.h          # Statistiche display
└── cli-stats.cpp        # Implementazione stats
```

## Comandi Disponibili

### Chat Commands
- `/exit` - Esci dall'applicazione
- `/clear` - Pulisci la cronologia chat
- `/regen` - Rigenera l'ultima risposta
- `/thinking` - Toggle modalità reasoning on/off
- `/stats` - Mostra statistiche dettagliate
- `/tools` - Lista tool disponibili
- `/tool <cmd>` - Gestione tool (add/remove/clear)

### File Commands
- `/read <file>` - Aggiungi contenuto file alla chat
- `/image <file>` - Aggiungi immagine (se supportato)
- `/audio <file>` - Aggiungi audio (se supportato)

## Tool Disponibili

### Default Tools
| Tool | Descrizione | Auto-Exec |
|------|-------------|-----------|
| `read_file` | Legge file | ✅ |
| `write_file` | Scrive file | ❌ (conferma se esiste) |
| `list_dir` | Lista directory | ✅ |
| `shell` | Esegue comandi shell | ❌ (sempre conferma) |

### Swift Tools
| Tool | Descrizione | Auto-Exec |
|------|-------------|-----------|
| `swift_build` | `swift build [-c release]` | ✅ |
| `swift_test` | `swift test [--filter X]` | ✅ |
| `swift_run` | `swift run [executable]` | ❌ |
| `swift_package` | `swift package <cmd>` | ❌ |
| `swift_format` | `swift format [--in-place]` | ✅ |

## Sicurezza

### Shell Command Whitelist
Comandi permessi:
- File: `ls`, `cat`, `grep`, `find`, `head`, `tail`, `wc`
- Swift: `swift`, `xcrun`, `xcodebuild`
- Git: `git`
- System: `pwd`, `whoami`, `uname`, `date`, `echo`

### Shell Command Blacklist (sempre bloccati)
- `rm -rf`, `sudo`, `su`, `chmod 777`
- `dd`, `mkfs`, `mount`, `umount`
- Pipe a shell: `curl | sh`, `wget | sh`
- Fork bomb e comandi pericolosi

### Conferme Richieste
- Scrittura file (se esiste già)
- Esecuzione comandi shell
- `swift run` (esecuzione codice)
- `swift package` (modifica dipendenze)
- Operazioni fuori dalla directory corrente

## Statistics Display

### Status Line (sempre visibile)
```
[..........] 12.5K/32K tokens Cache: +8.2K Tools: 3/20 45.2 t/s
```

### Detailed Stats (`/stats`)
```
=== Statistics ===
Context:     [████████....] 12543/32768 tokens (38%)
Cache save:  +8234 tokens
Generated:   1234 tokens (45.2 t/s)
Memory:      Model: 4.2 GB, KV: 256 MB
Tool calls:  3 (this turn), 15 (total)
Timing:      Prompt: 234ms, Generation: 567ms
```

## Tool Call Limit

- **Max 20 tool call per turno** (reset dopo ogni risposta all'utente)
- Previene loop infiniti con modelli piccoli
- Dopo il limite, il modello genera risposta normale

## Utilizzo

### Avvio Base
```bash
./llama-cli -m model.gguf --reasoning on
```

### Con Tool Personalizzati
```bash
# I tool sono caricati automaticamente
# Usa /tools per vedere la lista
# Usa /tool add <name> per riaggiungere un tool rimosso
```

### Esempio Sessione Swift
```
> Crea un nuovo package Swift e implementa una funzione hello

[Modello esegue: swift package init --name Hello]
[Modello esegue: read_file Sources/Hello/Hello.swift]
[Modello esegue: write_file Sources/Hello/Hello.swift (con nuovo contenuto)]
[Modello esegue: swift_build]
[Modello genera risposta finale]

> Esegui i test

[Modello esegue: swift_test]
```

## Token Budget Conservativo

- Tool definitions: inviate una volta sola (cache)
- Output tool: troncato a 4KB max
- Display output: troncato a 500 caratteri

## Estendere i Tool

### Aggiungere Tool Predefinito
In `cli-tool.cpp`:
```cpp
static const char* MIO_TOOL_SCHEMA = R"json({...})json";

std::vector<cli_tool> get_default_tools() {
    return {
        {"mio_tool", "Descrizione", MIO_TOOL_SCHEMA, false},
        // ...
    };
}
```

### Implementare Esecutore
In `cli-tool-exec.cpp`:
```cpp
cli_tool_result cli_tool_executor_impl::execute_mio_tool(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    // Implementazione...
    return result;
}
```

## Note Tecniche

- **C++17**: Usa `std::filesystem`, `std::async`
- **Thread-safe**: Esecuzione tool in async con timeout
- **Modulare**: Ogni componente è isolato e testabile
- **Compatibile**: Integrazione minimale con codice esistente

## TODO

- [ ] Persistenza tool tra sessioni
- [ ] Tool call in parallelo (parallel_tool_calls)
- [ ] Grammar constraining per tool call
- [ ] Supporto tool dinamici da config file
- [ ] Better error handling per timeout

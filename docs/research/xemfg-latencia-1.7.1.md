# Latência do XeMFG contra o DLSS-G (análise sem jogo, 1.7.1)

**O que se quer explicar:** Cyberpunk 2077 a 6x, mesma cena, cerca de 36 FPS reais. Latência de exibição p50 pelo PresentMon: DLSS-G 35,2 ms, XeMFG 59,6 a 61,5 ms. A diferença é de cerca de 24 ms, perto de um frame real (27,8 ms).

**Situação:** nada aqui foi medido em jogo. É uma análise do código e da `libxess_fg.dll`, lida sem carregar. A causa ainda não está identificada.

## O que o código mostra

- Depois de cada `Present` de saída nativa existe uma espera de até 1 s (`mov edx,3E8h` antes de uma chamada pela IAT em `0x18021F7C1`, argumento lido de `[rdi+160h]`). O comentário do upstream em `sdk_pacing.cpp` diz que é o waitable de latência da swapchain nativa, com latência de no máximo 3. Não confirmei o nome da função importada.
- Com o flip metering aceito e `MeteredQueueAhead=1` (padrão), `sdk_pacing.cpp` libera as saídas do lote na hora (`*output=now`) e o driver as espaça. Isso pode deixar até um lote na fila do driver.
- A espera na origem é o `xellSleep` em `SleepSourceFrame`, e o tempo dela não ficava registrado por período.

## Hipóteses, da mais para a menos provável

| # | Hipótese | Por que é plausível | O que confirma ou descarta |
|---|---|---|---|
| H1 | Fila de saídas à frente da tela (queue-ahead e latência nativa) | A 6x, um lote inteiro dura cerca de 28 ms, próximo da diferença | A diferença cai com `MeteredQueueAhead=0` ou `XeFGNativeLatency=1`, e encolhe a 2x |
| H2 | A XeLL não segura a fila na origem | Se a GPU está no limite e `sleep_us` fica perto de zero, um frame real pode estar esperando | `sleep_us` no `native_summary`; comparar com `[Latency] Output=Reflex` |
| H3 | Diferença da métrica | O PresentMon pode não associar os presents da XeFG ao frame real como faz com o DLSS-G | Sensação ao mover o mouse, ou captura em câmera lenta, discorda do número |
| H4 | Tempo de GPU da geração antes da primeira saída | A XeFG roda os passes antes de apresentar | Pequeno sozinho; a diferença cresce com a resolução, não só com o multiplicador |

A medição de 1.6 mostrou empate entre XeLL e Reflex como saída, o que enfraquece H2, mas ela não foi feita neste cenário.

## O que foi preparado

- **`native_summary`** agora traz `source_interval_us`, `sleep_us`, `present_call_us`, `latency_output`, `native_buffers`, `native_latency`, `flip_metered` e `queue_ahead`.
- **`native_chain_capacity`** registra uma vez os buffers e a latência real da swapchain da XeFG.
- **`[XeFG] XeFGNativeLatency`** muda a latência nativa. O padrão `0` não altera nada.

## Testes para quando houver jogo

Cyberpunk 2077, mesma cena, 30 s de PresentMon em cada linha. Reinicie o jogo depois de mudar o INI.

| # | INI | Se a latência cair |
|---|---|---|
| 1 | Nada (referência), a 6x | — |
| 2 | Multiplicador 2x | A diferença é da fila de saídas (H1) |
| 3 | `MeteredQueueAhead=0` | É o queue-ahead (H1) |
| 4 | `XeFGNativeLatency=1`, depois `2` | É a latência nativa (H1). Observe se o FPS e o pacing pioram |
| 5 | `[Latency] Output=Reflex` | É a XeLL (H2) |

Guarde o log do XeMFG de cada tomada. Nenhum padrão muda sem esse resultado.

# Plano 1.7.1 e 1.8.0 — XeMFG: robustez, latência, overhead e compatibilidade

Pedido: o que dá para melhorar no XeMFG em overhead, pacing, robustez e compatibilidade. Trabalho feito sem acesso a jogos (o usuário está fora): sem PresentMon nem campo nesta etapa, e nenhum padrão muda sem medição.

Status: `pendente`, `em andamento`, `feito`, `adiado`. **[você]** precisa de jogo.

## Bloco A — Robustez

| # | Tarefa | Status |
|---|---|---|
| A.1 | Espera de fence reconfere o valor depois do evento (dejitter, máscara de UI, scRGB, cópia de HUD) | feito |
| A.2 | Vulkan: estado das views de entrada por device, limpo ao destruir | feito |
| A.3 | Vulkan: sem vazamento de views no limite de 64; limite de imagens no log | feito |
| A.4 | Vulkan: private data apagado com a swapchain | feito |
| A.5 | Vulkan: IMMEDIATE só quando suportado; device nativo mantém o pedido do jogo | feito |
| A.6 | Vulkan: decisões em headers testáveis; smoke com device real, se rodar sem interação | parcial (headers feitos; smoke não) |
| A.7 | Vulkan: captura do DLSS não perde o frame enquanto o acquire dorme com o mutex | feito |
| A.8 | Manter o ReShade residente quando o XeMFG fixa o addon (RDR2 com RenoDX) | feito |

## Bloco B — Pacing e latência

| # | Tarefa | Status |
|---|---|---|
| B.1 | Latência do D3D12: análise do caminho, leitura estática da XeFG, telemetria e chave de latência nativa (padrão atual) | parcial (análise e telemetria feitas; causa pendente do B.5) |
| B.2 | Vulkan: sleep e markers de simulação do jogo guiam a XeLL, `[XeFG] VulkanGameLatency` desligado por padrão | feito (desligado; sem campo) |
| B.3 | Vulkan: limite do jogo e Display Cap | feito (sem campo) |
| B.4 | Dynamic MFG no Vulkan, desligado por padrão | feito (desligado, só INI; sem campo) |
| B.5 | **[você]** Matriz de latência e PresentMon no D3D12 e no Vulkan | adiado |

## Bloco C — Overhead

| # | Tarefa | Status |
|---|---|---|
| C.2 | Microajustes do caminho do Present | feito |
| C.1, C.3, C.4 | **[você]** Medir CPU, GPU e VRAM; otimizar só o que aparecer | adiado |

## Bloco D — Compatibilidade

| # | Tarefa | Status |
|---|---|---|
| D.1 | Vulkan: mais de um device vivo não bloqueia as entradas | feito (sem campo) |
| D.2 | Vulkan: motivo da volta ao nativo no painel e no relatório de compatibilidade | feito |
| D.3 | **[você]** Mais jogos Vulkan | adiado |
| D.4 | Máscara de HUD no Vulkan (opcional) | adiado |
| D.5 | D3D12: avaliações do DLSS-SR à frente dos Presents (RDR2): fila de frames | feito (falta campo) |
| D.6 | **[usuário do RDR2]** Campo do D.5 com build de teste | pendente |

## Decisões

1. A 1.7.1 vem primeiro; só sai depois do campo.
2. Limite de FPS no Vulkan: limite do jogo mais o Display Cap já existente, desligado por padrão.
3. Dynamic MFG: só no Vulkan, na 1.8.

## Registro

- **2026-10-30 · referência** Gate antes das mudanças: 197/197 (3 skipped de proveniência).
- **2026-10-30 · A.1–A.8, D.1, D.2** Feitos; testes novos `xemfg_fence_wait` e `xemfg_vulkan_policy`. O smoke Vulkan do A.6 não foi feito: os hooks ficam dentro do addon e dependem de ReShade como camada e de NGX Vulkan, o que pede um fixture próprio.
- **2026-10-30 · B, C.2** `VulkanGameLatency` e `VulkanDynamicMFG` desligados por padrão, `XeFGNativeLatency` com o padrão atual. Limite do jogo e Display Cap no Vulkan. Telemetria no `native_summary`; análise em `docs/research/xemfg-latencia-1.7.1.md`. Nada disso rodou em jogo.
- **2026-10-30 · gate** 199 testes, 197 passam; 3 skipped de proveniência. Falham `xemfg_conformance_exclusive-transition` e `-first` com `Present 0x887a0001`, porque o monitor desligou (área de trabalho em 1024x768 a 60 Hz, antes 239,8 Hz). O addon anterior às mudanças falha igual nessa condição. Repetir com o monitor ligado antes de qualquer release.
- **2026-10-31 · logs do RDR2** Usuário com RTX 4080, addon 1.7.0. Vulkan a 2x funciona, com e sem RenoDX. D3D12: geração em 2 de cada 5 frames, 40% dos frames com `no_matching_frame` e `id` 0. O adaptador só abria um frame depois do Present do anterior, então uma avaliação adiantada caía no mesmo `id` e um Present ficava sem frame.
- **2026-10-31 · D.5** `synthetic_queue.hpp`: fila de ids. Começa juntando avaliações (um frame avaliado duas vezes); dois Presents vazios depois de uma junção passam para um `id` por avaliação; fila acima de 4 volta a juntar para sempre. O sleep da XeLL fica depois do Present quando há frame aberto e, se a avaliação chegar antes, roda nela, sem o lock. Um Present sem frame não zera `o->last`, mas ainda desliga a XeFG naquele Present. Fixture com a ordem E E P P E E P P E P: 60/100 presents com geração antes, 100/100 depois; duas avaliações por frame, 20/20. A ordem real do RDR2 é inferida do log, não observada. `native_summary` traz `evaluations`, `merged_evaluations`, `empty_presents`, `pipelined`.
- **2026-10-31 · gate** Com o monitor ligado: 202/202 (3 skipped de proveniência), incluindo os dois testes de tela cheia exclusiva.

## Campo pendente, numa sessão só

1. RDR2 em D3D12 com build de teste (usuário do relato): geração estável, `pipelined` e `empty_presents` no log, latência percebida.
2. Vulkan, regressão: Indiana Jones e RDR2 com os padrões: geração ligada, troca de multiplicador, alt-tab, redimensionar, sair do jogo.
3. Vulkan, limite: com Limit to display refresh ligado, o painel mostra "Output limited to" e o FPS respeita o limite.
4. Vulkan, opcionais: `VulkanGameLatency=1` no Indiana Jones e `VulkanDynamicMFG=1`, cada um com PresentMon.
5. D3D12, regressão: Cyberpunk a 6x, e a matriz de latência de `docs/research/xemfg-latencia-1.7.1.md`.

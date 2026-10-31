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
| D.5 | D3D12: avaliações do DLSS-SR à frente dos Presents (RDR2): fila de frames | feito |
| D.6 | **[usuário do RDR2]** Campo do D.5 com build de teste | feito |

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
- **2026-10-31 · campo do D.5** RDR2 em D3D12, RTX 4070 SUPER, 3x, com RenoDX, cerca de 135 s de jogo. `native_synthetic_pipelined` cerca de 360 ms após ativar, com 15 avaliações, 5 juntadas e 5 Presents vazios; esses contadores não cresceram até o fim (9148 avaliações). `frames_without_fg` 0 em todas as janelas depois da primeira, sem `no_matching_frame` depois de ativar, `pipelined_refused` 0. Sessões 1.7.0 na mesma máquina chegaram a 40% sem geração, de forma intermitente. Sem atraso percebido; latência não medida. Os dois `pacing_reset` `delta_range` de cerca de 540 ms (09:26:30 e 09:26:43) foram o menu do jogo abrindo para salvar, segundo quem jogou.
- **2026-10-31 · congelamento em Vulkan** Indiana Jones, RTX 4070 SUPER, driver 32.0.16.2101, 6x: perder o foco (alt-tab ou clique fora) congela o jogo, às vezes na primeira perda, às vezes depois de várias. Reproduz na 1.7.0 (`3406F0AF`), então não é regressão da 1.7.1. Reproduz sem PresentMon e com `VulkanPresentMetering=0`. Com símbolos: `PresentThread` para em `CreateSwapchain`, no `vkCreateSwapchainKHR`, dentro do driver em `NtGdiDdDDIDestroyAllocation2`; o `Present1` do XeFG fica esperando o slot de staging. Sem efeito e revertido: `vkQueueWaitIdle` antes de recriar (travou na 5ª recriação). Sem `oldSwapchain`, a trava migra para `vkDestroySwapchainKHR`. Mitigação: o slot de staging é liberado antes de recriar (36 recreações até travar), o `Present1` descarta o frame durante a recriação (97) e não se cria swapchain `0x0` (37, sem ganho visível; fica porque o Vulkan proíbe esse tamanho). Uma sessão por build. O congelamento continua possível e a causa no driver não foi atribuída; RTSS e overlay da Steam não foram excluídos. Usuário decidiu seguir com este estado por enquanto. O watchdog de `VulkanFaults` passou a gravar até 256 threads e 160 endereços por pilha, porque os limites antigos cortavam a thread de present.
- **2026-10-31 · PresentMon Indiana Jones** Build `7B1B2C46`, 6x, 2560x1440, sessão de cerca de 4 min sem congelar, com 3 recreações de swapchain no início e 1 ao sair, PresentMon iniciado antes do jogo. 44856 intervalos: média 5,10 ms (196 FPS), p99 8,83 ms, p99,9 10,97 ms, 1% low 113 FPS, 80 intervalos acima de 9,70 ms. `DisplayLatency` p50 8,82 ms (não é latência de entrada a fóton). A captura inclui menu e carregamento; sem base comparável.
- **2026-10-31 · regressão Indiana Jones** Relato do usuário, na build `7B1B2C46`: multiplicador, redimensionar, sair e limite de FPS sem problema. O log confirma a saída e o limite configurado (232, não atingido); não mostra troca de multiplicador nem outro tamanho de janela nas sessões dessa build.
- **2026-10-31 · PresentMon Cyberpunk 2077** D3D12, build `7B1B2C46`, 6x, PresentMon iniciado antes do jogo. 30506 intervalos: média 4,70 ms (213 FPS), p99 6,88 ms, p99,9 8,71 ms, 1% low 145 FPS, 18 intervalos acima de 9,20 ms. `FrameType`: 5086 Application e 25428 Intel XeSS-FG (1:5). `DisplayLatency` p50 58,8 ms, sem base comparável e não comparável com a captura Vulkan do Indiana Jones.
- **2026-10-31 · Elden Ring** Relato de campo com a build de teste: com quedas fortes de FPS, a geração fica limpa e lisa. Sem log, e sem saber qual mudança da 1.7.1 causou a melhora.
- **2026-10-31 · Vulkan 1.7.0** No mesmo log, uma sessão teve `device fault: present thread submit` (`0x887a0005`) cerca de 200 ms após o takeover e voltou ao nativo (`vk_takeover_released`). As outras sessões Vulkan não tiveram isso. Sem causa atribuída. O item 2 do campo não roda RDR2 em Vulkan, então esta falha fica sem verificação na 1.7.1.

## Campo pendente, numa sessão só

1. RDR2 em D3D12 com build de teste: feito.
2. Vulkan, regressão: Indiana Jones com os padrões (RDR2 fora do escopo, por decisão de 2026-10-31): geração ligada, troca de multiplicador, alt-tab, redimensionar, sair do jogo.
3. Vulkan, limite: com Limit to display refresh ligado, o painel mostra "Output limited to" e o FPS respeita o limite.
4. Vulkan, opcionais: `VulkanGameLatency=1` no Indiana Jones e `VulkanDynamicMFG=1`, cada um com PresentMon.
5. D3D12, regressão: Cyberpunk a 6x, e a matriz de latência de `docs/research/xemfg-latencia-1.7.1.md`.

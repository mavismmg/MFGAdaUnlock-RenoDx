# Plano 1.7.1 e 1.8.0 — XeMFG: robustez, latência, overhead e compatibilidade

Pedido: o que dá para melhorar no XeMFG em overhead, pacing, robustez e compatibilidade. Trabalho feito sem acesso a jogos (o usuário está fora): sem PresentMon nem campo nesta etapa, e nenhum padrão muda sem medição.

Status: `pendente`, `em andamento`, `feito`, `adiado`. **[você]** precisa de jogo.

## Bloco A — Robustez

| # | Tarefa | Status |
|---|---|---|
| A.1 | Espera de fence reconfere o valor depois do evento (dejitter, máscara de UI, scRGB, cópia de HUD) | pendente |
| A.2 | Vulkan: estado das views de entrada por device, limpo ao destruir | pendente |
| A.3 | Vulkan: sem vazamento de views no limite de 64; limite de imagens no log | pendente |
| A.4 | Vulkan: private data apagado com a swapchain | pendente |
| A.5 | Vulkan: IMMEDIATE só quando suportado; device nativo mantém o pedido do jogo | pendente |
| A.6 | Vulkan: decisões em headers testáveis; smoke com device real, se rodar sem interação | pendente |
| A.7 | Vulkan: captura do DLSS não perde o frame enquanto o acquire dorme com o mutex | pendente |
| A.8 | Manter o ReShade residente quando o XeMFG fixa o addon (RDR2 com RenoDX) | feito |

## Bloco B — Pacing e latência

| # | Tarefa | Status |
|---|---|---|
| B.1 | Latência do D3D12: análise do caminho, leitura estática da XeFG, telemetria e chave de latência nativa (padrão atual) | pendente |
| B.2 | Vulkan: sleep e markers de simulação do jogo guiam a XeLL, `[XeFG] VulkanGameLatency` desligado por padrão | pendente |
| B.3 | Vulkan: limite do jogo e Display Cap | pendente |
| B.4 | Dynamic MFG no Vulkan, desligado por padrão | pendente |
| B.5 | **[você]** Matriz de latência e PresentMon no D3D12 e no Vulkan | adiado |

## Bloco C — Overhead

| # | Tarefa | Status |
|---|---|---|
| C.2 | Microajustes do caminho do Present | pendente |
| C.1, C.3, C.4 | **[você]** Medir CPU, GPU e VRAM; otimizar só o que aparecer | adiado |

## Bloco D — Compatibilidade

| # | Tarefa | Status |
|---|---|---|
| D.1 | Vulkan: mais de um device vivo não bloqueia as entradas | pendente |
| D.2 | Vulkan: motivo da volta ao nativo no painel e no relatório de compatibilidade | pendente |
| D.3 | **[você]** Mais jogos Vulkan | adiado |
| D.4 | Máscara de HUD no Vulkan (opcional) | adiado |

## Decisões

1. A 1.7.1 vem primeiro; só sai depois do campo.
2. Limite de FPS no Vulkan: limite do jogo mais o Display Cap já existente, desligado por padrão.
3. Dynamic MFG: só no Vulkan, na 1.8.

## Registro

- **2026-10-30 · referência** Gate antes das mudanças: 197/197 (3 skipped de proveniência).

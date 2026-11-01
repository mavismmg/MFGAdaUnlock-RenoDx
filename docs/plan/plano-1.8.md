# Plano 1.8 e 1.9 — XeMFG e DLSS-G: interface, performance, qualidade e modo XMX

Pedido de 2026-11-01: mais FPS em 1440p e 4K, melhorias vindas do motor do dashdogy, qualidade da geração, interface do XeMFG, interface do DLSS-G (principalmente submenus) e o `dlssg-to-xefg-xmx` como opção.

Status: `pendente`, `em andamento`, `feito`, `adiado`. **[você]** precisa de jogo ou decisão.

## Decisões de 2026-11-01

- Ordem: interface na 1.8, performance e qualidade na 1.9.
- 4K pode ser medido com DLDSR/DSR no monitor 1440p; o custo da geração é real, mas não vale como teste do monitor.
- Jogos de medição: Cyberpunk 2077 e Crimson Desert.
- Controles de teste do DLSS-G: não escondê-los nem removê-los; só melhorar a exibição, porque às vezes piscam.
- **XMX é um terceiro modo.** O CUDA continua; o usuário escolhe.
- Qualidade da geração terá os modos Qualidade, Normal e Desempenho, se a medição justificar cada um (pedido de 2026-11-01).

## Bloco 0 — Medir antes de otimizar

| # | Tarefa | Status |
|---|---|---|
| 0.1 | **[você]** Roteiro PresentMon: mesma cena, 2 jogos, 1440p e 4K, 2x/4x/6x, geração ligada e desligada | pendente |
| 0.2 | Custo de GPU por passe e VRAM, com o trace atual, ampliado se precisar | pendente |
| 0.3 | **[você]** Capturas de referência: movimento rápido, HUD, geometria fina | pendente |

## Bloco A — Performance em 1440p e 4K (1.9)

Cada item só entra se o bloco 0 mostrar que aquele custo pesa. Critério: ganho medido na mesma cena, sem piora visível nas capturas do 0.3.

| # | Hipótese | Status |
|---|---|---|
| A.1 | Resolução interna do fluxo óptico ou da rede em telas altas, se o SDK permitir | pendente |
| A.2 | Cópias de recursos e barreiras redundantes nas entradas e saídas | pendente |
| A.3 | Comparar CUDA e D3D12; filas e sincronização | pendente |
| A.4 | VRAM dos históricos em 4K | pendente |
| A.5 | Seletor de qualidade da geração, só com os modos que a medição justificar; Normal é o padrão. Hoje, Qualidade só teria candidato em 2x, e Desempenho não tem nenhum confirmado | pendente |
| A.6 | **[você]** Comparar o modelo 18 com o 16 em 2x: chave `XeFGStock2xModel=1`. Em 3x a 6x não há candidato: o 18 só gera o quadro do meio | código pronto, falta testar |

## Bloco B — Motor do dashdogy

| # | Tarefa | Status |
|---|---|---|
| B.1 | Comparar o que temos com a versão atual dele | feito: o motor já é a v1.7.0 (`b97deee`, plano J da 1.7), e depois dela só há um ajuste de README |
| B.2 | Listar o que vale portar: correção, desempenho, funcionalidade | feito: nada pendente de correção; flip queue já avaliada e desligada por padrão; DLSS 5 fora de escopo, sem pedido |
| B.3 | Portar um item por commit, com testes e `PROVENANCE.md` | adiado até haver versão nova |

## Bloco C — Qualidade da geração (1.9)

| # | Tarefa | Status |
|---|---|---|
| C.1 | Classificar artefatos por causa: vetores, profundidade, HUD ou limite do modelo | pendente |
| C.2 | Revisar convenções de câmera, dejitter e reset de histórico | pendente |
| C.3 | Testar parâmetros do modelo expostos pelo SDK e medir o custo | pendente |
| C.4 | HUD com o que já existe, sem reabrir o bloco I da 1.7 | pendente |

## Bloco D — Modo XMX

Referência: `realdody/dlssg-to-xefg-xmx`, que chama a API interna do XeFG (`Init`/`Execute`) sem o proxy de swapchain. **É GPLv3 e este projeto é MIT:** só as ideias; nada de código copiado. A parte do dashdogy é MIT e vem da fonte original.

| # | Tarefa | Status |
|---|---|---|
| D.1 | Relatório: rota direta contra a atual em compatibilidade, pacing, latência e perdas | feito: `docs/research/xemfg-modo-xmx-2026-11-01.md` |
| D.2 | Protótipo isolado, escrito do zero | em andamento: análise da 1.3.1 feita; sonda `[DLSSG] XmxProbe=1` pronta, sem execução. O gerador só começa depois do resultado dela |
| D.3 | Seletor de modo no painel e no INI: DLSS-G, XeMFG (CUDA), XMX; o padrão não muda | pendente |

## Bloco E — Interface do XeMFG (1.8)

| # | Tarefa | Status |
|---|---|---|
| E.1 | Inventário de opções: quem precisa, padrão, se reinicia o jogo | feito: 13 opções no painel, 36 chaves de INI |
| E.2 | Separar básico de avançado; nomes claros para "test" e "experimental" | separação já existe; renomear depende de decisão **[você]** |
| E.3 | Levar ao painel as opções só de INI, depois de validadas | adiado: `VulkanDynamicMFG` e `VulkanGameLatency` ainda sem medição em jogo |
| E.4 | Estado mais claro: gerando, pausado pelo jogo, motivo do bloqueio | já existe no painel (Hero e `status.detail`) |

## Bloco F — Interface do DLSS-G (1.8)

| # | Tarefa | Status |
|---|---|---|
| F.1 | Mapa das 5 páginas e dos 7 submenus | feito |
| F.2 | Reorganizar: nomes para usuário, menos níveis, mesma aparência do XeMFG | depende de decisão **[você]** |
| F.3 | Corrigir o piscar dos controles de teste e melhorar a exibição, mantendo todos visíveis | feito no código (`4f1f8b2`); falta confirmar em jogo **[você]** |
| F.4 | Comparar capturas do painel antes e depois | pendente |

Regra de E e F: nenhuma chave de INI muda de nome ou de padrão.

## Fora deste plano

Hotfix 1.7.3 do Dying Light 2, tratado à parte. O log da 1.7.2 mostra o jogo chamando `ResizeBuffers(1x1)` e liberando o swapchain antes do primeiro frame, sem recusa de interface.

## Validação

Bateria completa a cada bloco, regressão no Cyberpunk 2077 e no Crimson Desert, teste de campo antes de cada release.

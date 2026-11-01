# Modo XMX: a rota direta do XeFG como terceiro modo

Item D.1 do `docs/plan/plano-1.8.md`. Leitura de `realdody/dlssg-to-xefg-xmx` (build-3, commit `bcadaab`, 2026-11-01) e do nosso código na `feat/1.8`. Nada aqui foi executado nem medido: tudo sobre desempenho e compatibilidade é hipótese até o protótipo rodar.

## Como a rota direta funciona

1. A DLL substitui o `nvngx_dlssg.dll`, exportando as mesmas funções `NVSDK_NGX_D3D12_*`. Um carregador entrega essa DLL à NGX: hoje, o OptiScaler 10.
2. O Streamline, o plugin DLSS-G e o driver continuam donos da apresentação. **Não há proxy de swapchain nem XeLL.**
3. Em cada avaliação, o gerador copia backbuffer, profundidade, vetores e HUD-less e chama a API interna do XeFG (`CreateContext`, `Init`, `Execute`), gravando os quadros nas saídas que o DLSS-G apresenta.
4. Essa API não é exportada. Os endereços vêm de uma tabela por build (1.3.1 e 1.3.4) ou de uma varredura; as estruturas só foram verificadas nessas duas versões.
5. O limite de quadros e o modelo são trocados escrevendo nas configurações internas da `libxess_fg.dll`, e o modelo 16 vem do porte do dashdogy. O README diz que não altera código, mas ainda altera memória do processo.

## Comparação

| | XeMFG atual (CUDA) | Modo XMX |
|---|---|---|
| Quem apresenta | proxy XeFG, nosso flip metering | Streamline, DLSS-G e driver NVIDIA |
| Latência | XeLL, ou Reflex como opção | Reflex nativo |
| Modelo | 16 | 16 |
| Execução da rede | CUDA nos Tensor Cores, com shaders no restante | shaders; sem rota CUDA |
| Entradas | Streamline, FSR, DLSS-SR | só as que o DLSS-G recebe |
| Vulkan | sim | não |
| RTX 20/30 | sim, com a API sintética | a verificar: o Streamline recusa DLSS-G nessas placas |
| Dependência de versão | tabelas do host, verificadas | tabela ou varredura da API interna |

## O que esperar

- **Compatibilidade:** é o principal ganho potencial. Sem proxy, somem as diferenças de apresentação entre o XeMFG e o DLSS-G. Isso pode ajudar jogos como o Dying Light 2, mas o crash dele ainda não tem causa conhecida.
- **Pacing e latência:** passam a ser os do DLSS-G, que já medimos como bons. É hipótese que o XeFG se comporte igual nesse caminho.
- **Desempenho:** provavelmente não é a vantagem. O modelo é o mesmo, e sem CUDA a rede pode ficar mais lenta nas NVIDIA. Isso reforça a decisão de manter o CUDA.
- **CUDA no modo XMX:** a rota intercepta os dispatches da própria biblioteca, então talvez funcione também aqui. Não há evidência; vale testar no protótipo.

## Como encaixa no addon

- **Seletor:** `Kind` ganha um terceiro valor e `[Backend] Selected` aceita `XMX`. O padrão não muda, e os valores atuais continuam válidos.
- **Base:** o modo parte do backend DLSS-G, que já tem seletor de multiplicador, Reflex e hooks do Streamline.
- **Ponto de entrada:** o `loadhook.hpp` já detecta o `nvngx_dlssg` ao carregar. Com o modo XMX, as entradas `NVSDK_NGX_D3D12_*` desse módulo passam a ser as nossas. Assim o usuário não precisa de OptiScaler.
- **Conflito a resolver:** o backend DLSS-G aplica correções ao módulo da NVIDIA (o `gate` de arquitetura e o midpoint). No modo XMX, elas ficam desligadas.
- **Reuso:** `vendor/port/` e `vendor/runtime_payload` já são o porte MIT do dashdogy.

## Licença

O projeto de referência é GPLv3 e o nosso é MIT. Não copiamos código nem traduzimos arquivos dele. Os endereços e layouts da API interna devem vir da nossa própria análise das duas DLLs, e o registro fica em `PROVENANCE.md`. Isso é cuidado de engenharia, não parecer jurídico.

## Riscos

- Cada nova `libxess_fg.dll` pode exigir nova análise. Versão desconhecida deve cair para o modo atual, com aviso.
- Uma atualização do `nvngx_dlssg` pela NGX pode mudar parâmetros que o substituto precisa responder.
- Jogos que validam o `nvngx_dlssg` podem recusar a substituição.

## Próximos passos

| # | Tarefa | Precisa de você? |
|---|---|---|
| D.2a | Análise própria da API interna nas versões 1.3.1 e 1.3.4 | não |
| D.2b | `native_xmx`: entradas NGX, captura, `Execute`, modo no INI, desligado por padrão | não |
| D.2c | Teste no Streamline Sample real, que já está na bateria | só não estar jogando, porque abre janela |
| D.2d | Testes em jogo: Cyberpunk 2077, Crimson Desert, medição contra CUDA e DLSS-G; depois, Dying Light 2 com o Cpt.Hund | sim |
| D.3 | Seletor no painel, só depois do D.2d | decisão |

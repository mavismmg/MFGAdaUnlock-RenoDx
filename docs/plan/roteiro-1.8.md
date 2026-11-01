# Roteiro de campo da 1.8

Compara, na mesma cena: modelo 18 contra 16 em 2x, a rota XeMFG (CUDA), o DLSS-G e o modo XMX, em 1440p e em 4K com DLDSR. Cobre os itens 0.1 e 0.3 do plano e a parte de campo do D.2.

## Antes de começar

- Comandos a partir da raiz do repositório, com o jogo fechado, exceto a captura.
- Crimson Desert primeiro (`cd`). Cyberpunk 2077 depois (`cp`).
- **A mesma cena em todos os casos:** salve um ponto de partida e repita o mesmo trajeto de 30 s, com giro de câmera. Sem cutscene e sem menu aberto.
- Mesmas opções gráficas em todos os casos, com DLSS Super Resolution fixo. Não mude nada além da resolução.
- A captura pede confirmação do Windows (UAC) a cada execução. Os resultados ficam em `Documentos\MFGBenchmarks`.

## O ciclo de cada caso

```powershell
.\tools\mfg-field-test.ps1 -Game cd -Case xmx4x                 # jogo fechado: aplica o perfil
# abra o jogo, vá ao ponto de partida, deixe a geração como o comando indicou
.\tools\mfg-field-test.ps1 -Game cd -Case xmx4x -Capture 1440p  # inicie o trajeto logo depois do UAC
# feche o jogo, anote o que viu
```

## Casos

Em 1440p, todos, nesta ordem. Em 4K com DLDSR, só os marcados, porque o interesse ali é o custo da geração.

| Caso | O que é | 4K |
|---|---|---|
| `off` | Geração desligada no menu do jogo: a referência | sim |
| `xefg2x-18` | XeMFG 2x, modelo original 18 | |
| `xefg2x-16` | XeMFG 2x, nosso 16 | |
| `xmx2x` | XMX 2x, nosso 16 sem CUDA | |
| `dlssg4x` | DLSS-G 4x | sim |
| `xefg4x` | XeMFG 4x | sim |
| `xmx4x` | XMX 4x | sim |
| `xefg6x` | XeMFG 6x | |
| `xmx6x` | XMX 6x | |

Para 4K, ative o DLDSR no painel da NVIDIA, escolha a resolução 4K no jogo e use `-Capture 4k`.

## O que observar

Os números saem da captura. Só o olho responde o resto:

- **18 contra 16 em 2x:** contornos do personagem em giro rápido, HUD e legendas durante o movimento, grades, vegetação e cabos. Qual dos dois fica mais limpo?
- **XMX contra XeMFG:** a mesma imagem no mesmo multiplicador? Sente diferença de fluidez ou de resposta do controle?
- **Qualquer caso:** tela preta, tremida, travada, quadro fora de ordem (vai e volta), HUD fantasma.
- **Só no XMX:** Alt+Tab e mudança de resolução ou de modo de tela durante o jogo. Esse caminho ainda não foi exercitado em mudança de tamanho. Teste por último, porque pode derrubar o jogo.

## Se algo falhar

Anote o caso e guarde o `ReShade.log` da pasta do jogo antes de abri-lo de novo. Linhas com `xmx` dizem o que o gerador fez. `multi-frame model refused` significa que o XMX ficou limitado a 2x.

## Ao terminar

```powershell
.\tools\mfg-field-test.ps1 -Game cd -Restore
.\tools\mfg-field-test.ps1 -Game cp -Restore
```

Volta o addon e os dois INIs ao estado de antes dos testes.

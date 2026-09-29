# 逐个切换重采样比例,每按一次回车换到下一个,方便你直接听音高。
# 用法:先按住遥控器语音键说话,听到不对就按回车换下一个。
$ErrorActionPreference = "Stop"
$base = "http://192.168.31.4"

# num/den = 每秒输出的样本数 / 12000(遥控器实际送达率)
$ratios = @(
  @{n=1;   d=1;   note="1:1  直通(会缺25%并断续)"},
  @{n=16;  d=15;  note="1.067 极微慢"},
  @{n=6;   d=5;   note="1.200"},
  @{n=5;   d=4;   note="1.250"},
  @{n=9;   d=7;   note="1.286"},
  @{n=4;   d=3;   note="1.333 (当前,你说偏深沉)"},
  @{n=7;   d=5;   note="1.400"},
  @{n=3;   d=2;   note="1.500"},
  @{n=8;   d=5;   note="1.600"},
  @{n=5;   d=3;   note="1.667"},
  @{n=2;   d=1;   note="2.000 极快"}
)

function Show-Idx([int]$i) {
  $r = $ratios[$i]
  Write-Host ""
  Write-Host ("=" * 56) -ForegroundColor DarkGray
  Write-Host ("  [{0}/{1}]  {2}/{3}   ratio={4:N4}   {5}" -f ($i+1), $ratios.Count, $r.n, $r.d, ($r.n/$r.d), $r.note) -ForegroundColor Cyan
  Write-Host ("=" * 56) -ForegroundColor DarkGray
}

for ($i = 0; $i -lt $ratios.Count; $i++) {
  $r = $ratios[$i]
  try {
    $resp = Invoke-RestMethod -Uri "$base/api/audio/resample" -Method Post `
            -Body (@{num=$r.n; den=$r.d} | ConvertTo-Json) -ContentType "application/json" -TimeoutSec 6
  } catch {
    Write-Host "设置失败: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "设备地址可能变了,当前尝试 $base" -ForegroundColor Yellow
    exit 1
  }
  Show-Idx $i
  Write-Host "  已生效: num=$($resp.num) den=$($resp.d) ratio_pct=$($resp.ratio_pct) 每帧约 $($resp.outputs_per_frame_est) 样本"
  Write-Host "  现在说话,听音高和语速。"
  [void](Read-Host "  满意就记下这个比例告诉我;按回车试下一个")
}

Write-Host ""
Write-Host "全部试完。把你觉得最自然的那一个 num/den 告诉我。" -ForegroundColor Green

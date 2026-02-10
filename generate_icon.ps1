# Generate app.ico from iconHighRes.png
# Creates a multi-size .ico file (16, 32, 48, 256) from the high-res source

Add-Type -AssemblyName System.Drawing

$srcPath = Join-Path $PSScriptRoot "iconHighRes.png"
if (-not (Test-Path $srcPath)) {
    Write-Error "iconHighRes.png not found in $PSScriptRoot"
    exit 1
}

$srcImage = [System.Drawing.Image]::FromFile($srcPath)

$sizes = @(16, 32, 48, 256)
$images = @()
foreach ($size in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($srcImage, 0, 0, $size, $size)
    $g.Dispose()
    $images += $bmp
}

$srcImage.Dispose()

# Write .ico file
$outPath = Join-Path $PSScriptRoot "app.ico"
$stream = [System.IO.File]::Create($outPath)
$writer = New-Object System.IO.BinaryWriter($stream)

# ICO header
$writer.Write([uint16]0)       # Reserved
$writer.Write([uint16]1)       # Type: ICO
$writer.Write([uint16]$images.Count)

# Calculate data offset (header=6, entries=16 each)
$dataOffset = 6 + ($images.Count * 16)

# First pass: write directory entries
$pngDataList = @()
foreach ($i in 0..($images.Count - 1)) {
    $bmp = $images[$i]
    $size = $sizes[$i]

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngData = $ms.ToArray()
    $ms.Dispose()
    $pngDataList += ,$pngData

    $w = if ($size -ge 256) { 0 } else { $size }
    $h = if ($size -ge 256) { 0 } else { $size }
    $writer.Write([byte]$w)
    $writer.Write([byte]$h)
    $writer.Write([byte]0)        # Color palette
    $writer.Write([byte]0)        # Reserved
    $writer.Write([uint16]1)      # Color planes
    $writer.Write([uint16]32)     # Bits per pixel
    $writer.Write([uint32]$pngData.Length)
    $writer.Write([uint32]$dataOffset)
    $dataOffset += $pngData.Length
}

# Second pass: write image data
foreach ($pngData in $pngDataList) {
    $writer.Write($pngData)
}

$writer.Dispose()
$stream.Dispose()

foreach ($bmp in $images) { $bmp.Dispose() }

Write-Host "Created app.ico with sizes: $($sizes -join ', ')px from iconHighRes.png"

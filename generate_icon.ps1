# Generate app.ico for ReactionTime
# Creates a multi-size .ico file with a red circle and white "RT" text

Add-Type -AssemblyName System.Drawing

function New-IconImage {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    # Transparent background
    $g.Clear([System.Drawing.Color]::Transparent)

    # Red circle
    $blueBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 40, 100, 220))
    $g.FillEllipse($blueBrush, 0, 0, $Size - 1, $Size - 1)

    # White "RT" text
    $fontSize = [Math]::Max(6, [int]($Size * 0.42))
    $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold)
    $whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $rect = New-Object System.Drawing.RectangleF(0, 0, $Size, $Size)
    $g.DrawString("RT", $font, $whiteBrush, $rect, $sf)

    $g.Dispose()
    $font.Dispose()
    $blueBrush.Dispose()
    $whiteBrush.Dispose()
    $sf.Dispose()

    return $bmp
}

# Generate images at standard icon sizes
$sizes = @(16, 32, 48, 256)
$images = @()
foreach ($size in $sizes) {
    $images += New-IconImage -Size $size
}

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

    # Convert to PNG bytes
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngData = $ms.ToArray()
    $ms.Dispose()
    $pngDataList += ,$pngData

    # Directory entry
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

# Cleanup
foreach ($bmp in $images) { $bmp.Dispose() }

Write-Host "Created app.ico with sizes: $($sizes -join ', ')px"

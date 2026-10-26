package io.mooncheck.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

val Pine = Color(0xFF365C4B)
val Moss = Color(0xFFDCE8D9)
val Cream = Color(0xFFF6F7F2)
val Ink = Color(0xFF20352C)
private val light = lightColorScheme(primary = Pine, onPrimary = Color.White,
    primaryContainer = Moss, onPrimaryContainer = Ink, secondary = Color(0xFF8A6F3E),
    secondaryContainer = Color(0xFFF5E9CF), background = Cream, surface = Color(0xFFFEFEFA),
    surfaceVariant = Color(0xFFEBEEE6), onSurface = Ink, onSurfaceVariant = Color(0xFF627065),
    outline = Color(0xFFB3BEB1), error = Color(0xFFAF3F3F))
private val dark = darkColorScheme(primary = Color(0xFFB1D2B8), primaryContainer = Color(0xFF2E4D3D),
    secondaryContainer = Color(0xFF51452D), background = Color(0xFF111C17), surface = Color(0xFF19261F),
    surfaceVariant = Color(0xFF26372D), onSurface = Color(0xFFE2E9DF), onSurfaceVariant = Color(0xFFBBC7B9))
@Composable fun MoonTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (isSystemInDarkTheme()) dark else light,
        typography = Typography(
            headlineLarge = TextStyle(fontFamily = FontFamily.SansSerif, fontWeight = FontWeight.Bold, fontSize = 32.sp, lineHeight = 40.sp),
            headlineSmall = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 24.sp, lineHeight = 32.sp),
            titleLarge = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 20.sp, lineHeight = 28.sp),
            bodyLarge = TextStyle(fontSize = 16.sp, lineHeight = 25.sp),
            bodyMedium = TextStyle(fontSize = 14.sp, lineHeight = 22.sp)
        ), content = content)
}

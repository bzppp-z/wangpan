#pragma once

#include <QColor>

// 界面统一色板与尺寸（来源：阶段 2 界面设计契约）。
namespace theme {

extern const QColor kPrimary;          // #2D8CF0
extern const QColor kPrimaryHover;     // #1E6FD9
extern const QColor kPrimaryPressed;   // #1A5FB4
extern const QColor kPrimaryDisabled;  // #A0CFFF
extern const QColor kText;             // #1F2329
extern const QColor kTextMuted;        // #86909C
extern const QColor kBorder;           // #D9D9D9
extern const QColor kErrorText;        // #E64545
extern const QColor kErrorBg;          // #FFF1F0
extern const QColor kSuccess;          // #00B42A
extern const QColor kProgressTrack;    // #EBEEF5
extern const QColor kBrandFrom;        // #F5F8FF
extern const QColor kBrandTo;          // #EAF2FF

extern const int kWindowW;   // 760
extern const int kWindowH;   // 440
extern const int kBrandW;    // 320
extern const int kInputH;    // 40
extern const int kInputW;    // 360
extern const int kMargin;    // 40

extern const char* const kFontFamily;

}  // namespace theme

# 📁 Files Created/Modified

## 🆕 New Files (Created for You)

### Installation & Build
| File | Purpose |
|------|---------|
| **`INSTALL.bat`** | ⭐ **MAIN INSTALLER** - Double-click this to build & install |
| **`BuildAndDeploy.ps1`** | PowerShell script that does everything (build + deploy) |
| **`QUICKSTART.md`** | Quick reference card for installation |
| **`INSTALLATION_GUIDE.md`** | Detailed step-by-step guide |
| **`deploy_mod.ps1`** | Deploy-only script (copy files to game) |
| **`FILES_CREATED.md`** | This file - explains all files |

### Backup
| File | Purpose |
|------|---------|
| **`backup_stub_headers/`** | Old stub headers moved here (so real SDK is used) |

## 🔧 Modified Files (Updated for Your Setup)

### Build System
| File | Changes |
|------|---------|
| **`CMakeLists.txt`** | Updated to use SDK from Downloads folder, added SDK auto-detection, added missing libraries (comctl32, gdi32, shell32) |
| **`build.bat`** | Updated to use SDK from Downloads folder, added SDK verification |

## 📄 Documentation (Updated)
| File | Changes |
|------|---------|
| **`README.md`** | Complete rewrite with proper install instructions |
| **`SDK_FIX_SUMMARY.md`** | Explains the SDK path fix |

## 📦 Source Code (Unchanged)
| Folder | Contents |
|--------|----------|
| **`src/`** | All source code (.cpp and .h files) |
| **`bin/`** | Compiled binaries and required DLLs |

## 🗑️ Old/Temporary Files (Can be ignored)
These are build artifacts and old versions:
- `*.obj` files (build artifacts)
- `dlss4_*.cpp` files (old versions)
- `dxgi_proxy_*.cpp` files (old versions)
- `simple_proxy.cpp` (old version)
- `*.log` files (logs from previous runs)

---

## 🎯 What You Need to Use

### For Installation (Pick ONE):
1. **`INSTALL.bat`** ← Easiest, just double-click
2. **`BuildAndDeploy.ps1`** ← More control, run in PowerShell

### For Reference:
- **`README.md`** - Full documentation
- **`QUICKSTART.md`** - Quick cheat sheet
- **`INSTALLATION_GUIDE.md`** - Detailed troubleshooting

---

## 🚀 Quick Start (3 Steps)

1. **Download Streamline SDK**
   - https://developer.nvidia.com/rtx/streamline
   - Extract to Downloads

2. **Run Installer**
   - Double-click `INSTALL.bat`

3. **Play**
   - Launch game
   - Press F5 for menu

---

## 📋 File Checklist

Before running the installer, make sure you have:

- [ ] Downloaded this mod (all files)
- [ ] Downloaded NVIDIA Streamline SDK to Downloads folder
- [ ] Have AC Valhalla installed
- [ ] Have Visual Studio 2022 OR Build Tools installed

---

## ❓ FAQ

**Q: Which file do I run?**
A: Double-click `INSTALL.bat`

**Q: Do I need to edit any files?**
A: No! Everything is configured automatically.

**Q: What if the installer can't find my game?**
A: Run: `.\BuildAndDeploy.ps1 -CustomGamePath "C:\Your\Path"`

**Q: What if the SDK is in a different location?**
A: The script auto-detects any `streamline-sdk*` folder in Downloads. If yours is elsewhere, edit the path in `CMakeLists.txt` or `build.bat`.

**Q: Can I just copy files manually?**
A: Yes! Copy all files from `bin\` to your AC Valhalla folder (but you still need to build first).

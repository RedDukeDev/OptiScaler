#!/usr/bin/env bash

show_help() {
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --filename=<string>        Set target filename for OptiScaler.dll. Options are:"
    echo "                            - dxgi.dll"
    echo "                            - winmm.dll"
    echo "                            - version.dll"
    echo "                            - dbghelp.dll"
    echo "                            - d3d12.dll"
    echo "                            - wininet.dll"
    echo "                            - winhttp.dll"
    echo "                            - OptiScaler.asi"
    echo ""
    echo "  --overwrite=<y|n>       Overwrite existing file (y/n)"
    echo "  --using_nvidia=<y|n>    Using Nvidia GPU (y/n)"
    echo "  --using_dlss=<y|n>      Use DLSS inputs/spoofing (y/n)"
    echo "  --bridge=<y|n>          For a 32-bit game: install the 32-bit bridge (y/n)"
    echo "  -h, --help              Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 --filename=dxgi.dll --overwrite=y --using_nvidia=n --using_dlss=y"
    echo ""
    exit 0
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) show_help ;;
        --filename=*) selected_filename="${arg#*=}" ;;
        --overwrite=*) overwrite_choice="${arg#*=}" ;;
        --using_nvidia=*) using_nvidia="${arg#*=}" ;;
        --using_dlss=*) using_dlss="${arg#*=}" ;;
        --bridge=*) bridge_choice="${arg#*=}" ;;
    esac
done
clear

echo " ::::::::  :::::::::  ::::::::::: :::::::::::  ::::::::   ::::::::      :::     :::        :::::::::: :::::::::  "
echo ":+:    :+: :+:    :+:     :+:         :+:     :+:    :+: :+:    :+:   :+: :+:   :+:        :+:        :+:    :+: "
echo "#+:    +:+ +:+    +:+     +:+         +:+     +:+        +:+         +:+   +:+  +:+        +:+        +:+    +:+ "
echo "+#+    +:+ +#++:++#+      +#+         +#+     +#++:++#++ +#+        +#++:++#++: +#+        +#++:++#   +#++:++#:  "
echo "+#+    +#+ +#+            +#+         +#+            +#+ +#+        +#+     +#+ +#+        +#+        +#+    +#+ "
echo "#+#    #+# #+#            #+#         #+#     #+#    #+# #+#    #+# #+#     #+# #+#        #+#        #+#    #+# "
echo " ########  ###            ###     ###########  ########   ########  ###     ### ########## ########## ###    ### "
echo ""
echo "Coping is strong with this one..."
echo ""

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPTISCALER_FILE="$SCRIPT_DIR/OptiScaler.dll"

# Remove junk files
rm -f "$SCRIPT_DIR/!! EXTRACT ALL FILES TO GAME FOLDER !!" 2>/dev/null
rm -f "$SCRIPT_DIR/setup_windows.bat" 2>/dev/null

# Check if OptiScaler.dll exists
if [ ! -f "OptiScaler.dll" ]; then
    echo "OptiScaler \"OptiScaler.dll\" file is not found!"
    echo "Please make sure you extracted all OptiScaler files to the game folder."
    echo ""
    echo "For Unreal Engine games, look for the game executable in:"
    echo "- <path-to-game>/Game-or-Project-name/Binaries/Win64/"
    echo "- Ignore the Engine folder"
    echo ""
    read -p "Press Enter to exit..."
    exit 1
fi

# Unreal Engine detection (skip for headless)
if [ -d "$SCRIPT_DIR/Engine" ] && [ -z "$selected_filename" ]; then
    echo "Found Engine folder, if this is an Unreal Engine game then please extract OptiScaler to #CODENAME#/Binaries/Win64"
    echo ""

    while true; do
        read -p "Continue installation to current folder? [y/n]: " continue_choice
        continue_choice=$(echo "$continue_choice" | tr -d ' ')

        if [ "$continue_choice" = "y" ] || [ "$continue_choice" = "Y" ]; then
            break
        elif [ "$continue_choice" = "n" ] || [ "$continue_choice" = "N" ]; then
            echo "Installation cancelled."
            read -p "Press Enter to exit..."
            exit 0
        fi
    done
fi

select_filename() {
    # Skip if filename already set and valid
    if [ -n "$selected_filename" ]; then
        case "$selected_filename" in
            dxgi.dll|winmm.dll|version.dll|dbghelp.dll|d3d12.dll|wininet.dll|winhttp.dll|OptiScaler.asi)
                return
                ;;
            *)
                echo "Invalid filename: $selected_filename"
                exit 1
                ;;
        esac
    fi
    
    while true; do
        echo ""
        echo "Choose a filename for OptiScaler (default is dxgi.dll):"
        echo " [1] dxgi.dll"
        echo " [2] winmm.dll"
        echo " [3] version.dll"
        echo " [4] dbghelp.dll"
        echo " [5] d3d12.dll"
        echo " [6] wininet.dll"
        echo " [7] winhttp.dll"
        echo " [8] OptiScaler.asi"

        read -p "Enter 1-8 (or press Enter for default): " filename_choice

        case "$filename_choice" in
            ""|"1")
                selected_filename="dxgi.dll"
                ;;
            "2")
                selected_filename="winmm.dll"
                ;;
            "3")
                selected_filename="version.dll"
                ;;
            "4")
                selected_filename="dbghelp.dll"
                ;;
            "5")
                selected_filename="d3d12.dll"
                ;;
            "6")
                selected_filename="wininet.dll"
                ;;
            "7")
                selected_filename="winhttp.dll"
                ;;
            "8")
                selected_filename="OptiScaler.asi"
                ;;
            *)
                clear
                echo "Invalid choice. Please select a valid option."
                echo ""
                continue
                ;;
        esac

        # Check if file already exists
        if [ -f "$selected_filename" ]; then
            echo ""
            echo "WARNING: $selected_filename already exists in the current folder."
            echo ""
            
            if [ -n "$overwrite_choice" ]; then
                if [[ "$overwrite_choice" =~ ^(yes|y)$ ]]; then
                    break
                else
                    echo "File exists and overwrite_choice=$overwrite_choice, exiting."
                    exit 1
                fi
            fi

            while true; do
                read -p "Do you want to overwrite it? [y/n]: " overwrite_choice
                overwrite_choice=${overwrite_choice,,} 

                if [[ "$overwrite_choice" =~ ^(yes|y)$ ]]; then
                    break 2  # Break out of both loops
                elif [[ "$overwrite_choice" =~ ^(no|n)$ ]]; then
                    clear
                    break  # Break out of the inner loop, continue filename selection
                else
                    clear
                    echo "Invalid choice. Please enter 'y' or 'n'."
                fi
            done
	    else
            break  # File doesn't exist, proceed
        fi
    done
}

# A 32-bit game cannot load OptiScaler, which is 64-bit. For those the package carries a bridge (bridge32/): a
# 32-bit d3d9.dll and dxgi.dll that hand the game's frames to a 64-bit helper that runs the network. The helper
# is a Windows process of its own, in the same Wine prefix as the game.

# The machine type of a PE file: 014c is 32-bit, 8664 is 64-bit.
pe_machine() {
    local offset
    offset=$(od -An -t u4 -j 60 -N 4 "$1" 2>/dev/null | tr -d ' ')
    [ -n "$offset" ] || return 1
    od -An -t x2 -j $((offset + 4)) -N 2 "$1" 2>/dev/null | tr -d ' '
}

# 64 if any executable in the folder is 64-bit, 32 if they are all 32-bit, ? if there are none to tell.
game_bits() {
    local f m seen32=0
    for f in "$SCRIPT_DIR"/*.exe; do
        [ -f "$f" ] || continue
        m=$(pe_machine "$f")
        case "$m" in
            8664) echo 64; return ;;
            014c) seen32=1 ;;
        esac
    done
    if [ "$seen32" = 1 ]; then echo 32; else echo '?'; fi
}

bridge_mode=0
if [ -f "$SCRIPT_DIR/bridge32/d3d9.dll" ] && [ "$(game_bits)" = "32" ]; then
    if [ -z "$bridge_choice" ]; then
        echo ""
        echo "This game is 32-bit. OptiScaler itself is 64-bit and cannot run inside it, but a small 32-bit part can"
        echo "hand the game's frames to a 64-bit helper that runs the network. It works with Direct3D 9 and Direct3D 11"
        echo "games, and has its own menu in the game (Insert), with the settings kept in OptiScaler.ini."
        echo ""
        read -r -p "Install the 32-bit bridge? [Y/n]: " bridge_choice
        bridge_choice=${bridge_choice:-y}
    fi
    case "${bridge_choice,,}" in
        y|yes) bridge_mode=1; selected_filename="OptiScaler.dll" ;;
    esac
fi

if [ "$bridge_mode" != "1" ]; then

select_filename

# Try to detect Nvidia
NVIDIA_DETECTED=false
if command -v nvidia-smi >/dev/null 2>&1; then
    if nvidia-smi >/dev/null 2>&1; then
        NVIDIA_DETECTED=true
        echo "Nvidia GPU detected."
    fi
fi

while [ -z "$using_nvidia" ]; do
    echo ""
    if [ "$NVIDIA_DETECTED" = true ]; then
        default_value="y"
        read -r -p "Are you using an Nvidia GPU [Y/n]: " using_nvidia
    else
        default_value="n"
        read -r -p "Are you using an Nvidia GPU [y/N]: " using_nvidia
    fi

    using_nvidia=${using_nvidia,,}
    using_nvidia=${using_nvidia:-$default_value}
    
    if [[ "$using_nvidia" =~ ^(no|n)$ ]]; then
        while [ -z "$using_dlss" ]; do
            echo ""
            read -r -p "Will you try to use DLSS inputs? (enables spoofing, required for DLSS FG, Reflex->AL2) [Y/n]: " using_dlss

            using_dlss=${using_dlss,,}
            using_dlss=${using_dlss:-y}

            if [[ "$using_dlss" =~ ^(no|n)$ ]]; then
                # Disable spoofing
                config_file="OptiScaler.ini"
                if [ ! -f "$config_file" ]; then
                    echo "Config file not found: $config_file"
                    read -p "Press Enter to continue..."
                else
                    # Use sed to replace Dxgi=auto with Dxgi=false
                    sed -i 's/Dxgi=auto/Dxgi=false/g' "$config_file"
                    echo "Spoofing disabled in configuration."
                fi
                break
            elif [[ "$using_dlss" =~ ^(yes|y)$ ]]; then
                break
            else
                echo "Invalid choice. Please enter 'y' or 'n'."
                continue
            fi
        done
        break
    elif [[ "$using_nvidia" =~ ^(yes|y)$ ]]; then
        break
    else
        echo "Invalid choice. Please enter 'y' or 'n'."
        continue
    fi
done

# Rename OptiScaler file
echo ""
if [ "$overwrite_choice" = "y" ] || [ "$overwrite_choice" = "Y" ]; then
    echo "Removing previous $selected_filename..."
    rm -f "$selected_filename"
fi

echo "Renaming OptiScaler file to $selected_filename..."
if ! mv "$OPTISCALER_FILE" "$selected_filename"; then
    echo ""
    echo "ERROR: Failed to rename OptiScaler file to $selected_filename."
    echo "Please check file permissions and try again."
    read -p "Press Enter to exit..."
    exit 1
fi

fi   # bridge_mode != 1

# Neural rendering (dlss5nr) on Linux: the Windows half of its HIP bridge has to
# be beside the game's executable, where Wine looks for it first. The rest of
# what it needs stays in linux/ (see linux/README.txt).
install_hip_bridge() {
    local bridge="$SCRIPT_DIR/linux/amdhip64_7.dll"
    [ -f "$bridge" ] || return 0

    # A package assembled or extracted somewhere that does not keep the
    # executable bit leaves the launcher unable to start (Steam: exit code 126)
    chmod +x "$SCRIPT_DIR/linux/launch.sh" 2>/dev/null

    echo ""
    if [ -f "$SCRIPT_DIR/amdhip64_7.dll" ] && ! cmp -s "$bridge" "$SCRIPT_DIR/amdhip64_7.dll"; then
        echo "WARNING: amdhip64_7.dll already exists here and is not the one from the linux folder."
        echo "Leaving it alone; neural rendering needs the one in linux/ beside the executable."
        return 0
    fi
    if cp -f "$bridge" "$SCRIPT_DIR/amdhip64_7.dll"; then
        echo "Installed amdhip64_7.dll (HIP bridge for neural rendering)."
    else
        echo "WARNING: could not copy linux/amdhip64_7.dll beside the executable."
    fi
}

[ "$bridge_mode" != "1" ] && install_hip_bridge

# One part of the bridge for each of the two ways a game can draw: d3d9.dll for Direct3D 9, dxgi.dll for Direct3D
# 10 and 11. The game loads whichever it uses, so there is no need to know which. A DLL of that name that was there
# is kept as <name>_chain.dll, which the bridge loads in its place.
install_bridge_client() {
    local name="$1"
    local source="$SCRIPT_DIR/bridge32/$name.dll"
    [ -f "$source" ] || return 0
    if [ -f "$SCRIPT_DIR/$name.dll" ] && ! cmp -s "$source" "$SCRIPT_DIR/$name.dll"; then
        if [ -f "$SCRIPT_DIR/${name}_chain.dll" ]; then
            echo ""
            echo "WARNING: $name.dll and ${name}_chain.dll are both here already, so $name.dll was left alone."
            echo "Rename or remove one of them and run this again."
            return 1
        fi
        mv "$SCRIPT_DIR/$name.dll" "$SCRIPT_DIR/${name}_chain.dll"
        echo "The $name.dll that was here is now ${name}_chain.dll: the bridge loads it."
    fi
    if cp -f "$source" "$SCRIPT_DIR/$name.dll"; then
        echo "Installed $name.dll"
    else
        echo "ERROR: could not copy $name.dll."
        return 1
    fi
}

install_bridge() {
    echo ""
    echo "Installing the 32-bit bridge..."
    install_bridge_client d3d9 || return 1
    install_bridge_client dxgi || return 1
    # The neural rendering is on from the start, in the section the bridge reads.
    if [ -f OptiScaler.ini ]; then
        sed -i '/^\[NeuralRendering\]/,/^\[/ s/^Enabled=auto/Enabled=true/' OptiScaler.ini
    fi
    # The 64-bit helper is a process of its own: the Windows half of the HIP bridge has to be where it looks, beside it.
    if [ -f "$SCRIPT_DIR/linux/amdhip64_7.dll" ] && [ -d "$SCRIPT_DIR/OptiScaler/dlss5nr" ]; then
        cp -f "$SCRIPT_DIR/linux/amdhip64_7.dll" "$SCRIPT_DIR/OptiScaler/dlss5nr/amdhip64_7.dll" ||
            echo "WARNING: could not copy amdhip64_7.dll beside the helper."
    fi
    chmod +x "$SCRIPT_DIR/linux/launch.sh" 2>/dev/null
    # OptiScaler.dll itself is not used in a 32-bit game.
    rm -f "$SCRIPT_DIR/OptiScaler.dll"
    echo ""
    echo "The bridge is installed."
}

if [ "$bridge_mode" = "1" ]; then
    if ! install_bridge; then
        read -p "Press Enter to exit..."
        exit 1
    fi
fi

# Create uninstaller
create_uninstaller() {
    cat > "remove_optiscaler.sh" << 'EOF'
#!/usr/bin/env bash

show_help() {
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --remove=<y|n>    Confirm removal (y/n)"
    echo "  -h, --help        Show this help message"
    echo ""
    exit 0
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) show_help ;;
        --remove=*) remove_choice="${arg#*=}" ;;
    esac
done

clear
echo " ::::::::  :::::::::  ::::::::::: :::::::::::  ::::::::   ::::::::      :::     :::        :::::::::: :::::::::  "
echo ":+:    :+: :+:    :+:     :+:         :+:     :+:    :+: :+:    :+:   :+: :+:   :+:        :+:        :+:    :+: "
echo "#+:    +:+ +:+    +:+     +:+         +:+     +:+        +:+         +:+   +:+  +:+        +:+        +:+    +:+ "
echo "+#+    +:+ +#++:++#+      +#+         +#+     +#++:++#++ +#+        +#++:++#++: +#+        +#++:++#   +#++:++#:  "
echo "+#+    +#+ +#+            +#+         +#+            +#+ +#+        +#+     +#+ +#+        +#+        +#+    +#+ "
echo "#+#    #+# #+#            #+#         #+#     #+#    #+# #+#    #+# #+#     #+# #+#        #+#        #+#    #+# "
echo " ########  ###            ###     ###########  ########   ########  ###     ### ########## ########## ###    ### "
echo ""
echo "Coping is strong with this one..."
echo ""

if [ -z "$remove_choice" ]; then
    read -p "Do you want to remove OptiScaler? [y/n]: " remove_choice
fi

if [ "$remove_choice" = "y" ] || [ "$remove_choice" = "Y" ]; then
    echo ""
    echo "Removing OptiScaler files..."
    
    # Remove OptiScaler files
    rm -f OptiScaler.log
    rm -f OptiScaler.ini
    rm -f SELECTED_FILENAME_PLACEHOLDER
    rm -f fakenvapi.dll
    rm -f fakenvapi.ini
    rm -f fakenvapi.log
    rm -f dlssg_to_fsr3_amd_is_better.dll
    rm -f dlssg_to_fsr3.log
    
    # Remove directories
    rm -rf D3D12_Optiscaler
    rm -rf DlssOverrides
    rm -rf Licenses

    # Neural rendering (dlss5nr) lives in the OptiScaler folder, with the
    # rest of OptiScaler's own libraries; an older package put it beside the
    # game's executable instead
    rm -rf OptiScaler/dlss5nr
    if [ -f dlss5nr/dlss5nr_runtime.dll ]; then
        rm -rf dlss5nr
    fi
    rm -rf OptiScaler

    # The HIP bridge setup_linux.sh copied beside the executable: only if it is
    # still the one from the linux folder
    if [ -f amdhip64_7.dll ] && [ -f linux/amdhip64_7.dll ] && cmp -s amdhip64_7.dll linux/amdhip64_7.dll; then
        rm -f amdhip64_7.dll
    fi
    if [ -f linux/launch.sh ]; then
        rm -rf linux
    fi

    # The 32-bit bridge, if it was installed: the DLLs that are still the package's own are removed, and what
    # was there before comes back
    for name in d3d9 dxgi; do
        if [ -f "bridge32/$name.dll" ] && [ -f "$name.dll" ] && cmp -s "$name.dll" "bridge32/$name.dll"; then
            rm -f "$name.dll"
            if [ -f "${name}_chain.dll" ]; then
                mv "${name}_chain.dll" "$name.dll"
            fi
        fi
    done
    rm -f dlss5nr_bridge.log dlss5nr_bridge_dxgi.log
    rm -rf bridge32
    
    echo ""
    echo "OptiScaler removed!"
    echo ""
    
    # Remove this uninstaller
    rm -f "$0"
else
    echo ""
    echo "Operation cancelled."
    echo ""
fi

if [ $# -eq 0 ]; then
    read -p "Press Enter to exit..."
fi
EOF

    # Replace the placeholder with the actual selected filename
    sed -i "s/SELECTED_FILENAME_PLACEHOLDER/$selected_filename/g" "remove_optiscaler.sh"
    
    # Make the uninstaller executable
    chmod +x "remove_optiscaler.sh"
    
    echo ""
    echo "Uninstaller created: remove_optiscaler.sh"
    echo ""
}

# Create the uninstaller
create_uninstaller

# Success message
clear
echo " OptiScaler setup completed successfully..."
echo ""
echo "  ___                 "
echo " (_         '        "
echo " /__  /)   /  () (/  "
echo "         _/      /    "
echo ""

# Display Wine DLL override information
echo "IMPORTANT FOR LINUX/WINE USERS:"
if [ "$bridge_mode" = "1" ]; then
    echo "The bridge is d3d9.dll and dxgi.dll beside the game: Wine has to prefer them to its own. If using Steam,"
    echo "add this to launch options:"
    echo ""
    if [ -f "$SCRIPT_DIR/linux/launch.sh" ]; then
        echo "WINEDLLOVERRIDES=\"d3d9=n,b;dxgi=n,b\" \"$SCRIPT_DIR/linux/launch.sh\" %COMMAND%"
    else
        echo "WINEDLLOVERRIDES=\"d3d9=n,b;dxgi=n,b\" %COMMAND%"
    fi
    echo ""
    echo "Remember: in the game, the Insert key opens the bridge's menu (see bridge32/README.txt)"
    echo ""
else
echo "You might need to add the renamed DLL to Wine overrides"
echo "Example, if using Steam, add this to launch options:"
echo ""
echo "WINEDLLOVERRIDES=$selected_filename=n,b %COMMAND%"
echo ""
if [ -f "$SCRIPT_DIR/linux/launch.sh" ]; then
    echo "For neural rendering, run the game through the launcher instead (see linux/README.txt):"
    echo ""
    echo "WINEDLLOVERRIDES=$selected_filename=n,b \"$SCRIPT_DIR/linux/launch.sh\" %COMMAND%"
    echo ""
fi
echo "Remember: Insert key opens OptiScaler overlay, Page Up/Down for performance stats"
echo ""
fi

# Cleanup - remove setup script
if [ $# -eq 0 ]; then
    read -p "Press Enter to exit..."
fi

rm -f "$0"

exit 0

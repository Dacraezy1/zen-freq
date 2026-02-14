# Contributing to zen-freq

Thank you for considering contributing to zen-freq!

## Development Setup

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install build-essential linux-headers-$(uname -r) flex bison libelf-dev

# Fedora
sudo dnf install kernel-devel kernel-headers gcc make flex bison

# Arch Linux
sudo pacman -S linux-headers base-devel
```

### Building

```bash
make                # Build module
make clean          # Clean artifacts
sudo insmod zen-freq.ko  # Load for testing
sudo rmmod zen-freq      # Unload
```

## Code Style

Follow Linux kernel coding style:

- Use tabs for indentation (8 spaces)
- Functions: return type on separate line
- Use `pr_info`, `pr_debug`, `pr_err` for logging
- Document all exported functions

## Pull Request Process

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit pull request

### PR Checklist

- [ ] Code follows kernel style
- [ ] Functions are documented
- [ ] Module loads/unloads cleanly
- [ ] Tested on AMD hardware

## Testing

```bash
# Load module
sudo insmod zen-freq.ko

# Check sysfs
ls /sys/kernel/zen_freq/

# Monitor
watch -n 1 "cat /sys/kernel/zen_freq/temperature"

# Unload
sudo rmmod zen-freq
```

## Questions?

Open an issue with the "question" label.

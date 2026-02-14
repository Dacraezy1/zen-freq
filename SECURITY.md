# Security Policy

## Supported Versions

| Version | Supported |
| ------- | --------- |
| 2.x     | ✅        |
| 1.x     | ❌        |

## Reporting a Vulnerability

**Do not report security vulnerabilities through public GitHub issues.**

Instead, use GitHub Security Advisories:
1. Go to [Security Advisories](https://github.com/Dacraezy1/zen-freq/security/advisories)
2. Click "Report a vulnerability"
3. Provide details

## Security Features

### Voltage Protection
- P-states exceeding 1.45V are clamped
- Prevents electromigration damage

### Thermal Protection
- Soft throttle at 80°C
- Hard throttle at 90°C
- Automatic recovery

### Access Control
- Module loading requires root
- Sysfs interface root-only
- No user-writable dangerous interfaces

## Safe Usage

1. Build from source or use official releases
2. Verify checksums
3. Monitor `/sys/kernel/zen_freq/` for status

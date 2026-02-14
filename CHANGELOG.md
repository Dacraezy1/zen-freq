# Changelog

All notable changes to this project will be documented in this file.

## [2.0.0] - 2024

### Added
- **Zero-IPI Frequency Transitions** - Uses smp_call_function_single() for local MSR writes
- **Thermal Guard with PI Controller** - Background kthread with proportional-integral control
- **I/O Wait Performance Boost** - Automatic boost during I/O wait states
- **Lock-less Fast Switch** - RCU-protected, completely mutex-free
- **Voltage Safety Verification** - VID monitoring and clamping for silicon health
- **Dynamic EPP Tuning** - Automatic adjustment based on CPU utilization

### Technical Details
- RCU-protected frequency tables
- Atomic performance target updates
- Anti-windup integral controller
- Sysfs interface at /sys/kernel/zen_freq/

## [1.0.0] - 2024

### Added
- Initial release
- Basic P-state control
- Boost frequency support
- CPPC-free operation

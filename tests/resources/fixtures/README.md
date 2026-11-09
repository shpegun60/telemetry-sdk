# Resource facade fixture

These process-lived Model bindings and Descriptor/Values providers support
`tests/resources/DeviceCheck.cpp`. The structured Resource runner compiles the
two implementation files directly, then tests the generic facade without Qt.

They were retained from the retired Widgets playground to preserve provider
composition and byte-stream coverage. They are test inputs, not installed SDK
modules or an application to link into a project. See the independent
[device integration example](../../../examples/device_integration/README.md)
for application ownership and the [test runner](../../structured/resources/run.py)
for reproduction.

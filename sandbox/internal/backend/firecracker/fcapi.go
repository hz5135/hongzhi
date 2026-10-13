package firecracker

import (
	"context"
	"fmt"
	"time"

	firecracker "github.com/firecracker-microvm/firecracker-go-sdk"
	sdkclient "github.com/firecracker-microvm/firecracker-go-sdk/client"
	"github.com/firecracker-microvm/firecracker-go-sdk/client/models"
	"github.com/firecracker-microvm/firecracker-go-sdk/client/operations"

	"agentbench/sandbox/internal/sbx"
)

// fcAPI wraps the firecracker-go-sdk typed client over the jailer's Unix
// socket. All Firecracker VM configuration goes through the SDK here (no raw
// HTTP / no command line).
type fcAPI struct {
	c *sdkclient.Firecracker
}

func newFCAPI(socketPath string) *fcAPI {
	transport := firecracker.NewUnixSocketTransport(socketPath, nil, false)
	return &fcAPI{c: sdkclient.New(transport, nil)}
}

func waitForSocket(path string, timeoutSec int) bool {
	deadline := time.Now().Add(time.Duration(timeoutSec) * time.Second)
	for time.Now().Before(deadline) {
		if sbx.FileExists(path) {
			return true
		}
		// The API socket is created within milliseconds of the jailer exec;
		// poll finely so startup is not padded by a full-second sleep.
		time.Sleep(10 * time.Millisecond)
	}
	return false
}

func (a *fcAPI) putBootSource(ctx context.Context, kernelPath, bootArgs string) error {
	_, err := a.c.Operations.PutGuestBootSource(&operations.PutGuestBootSourceParams{
		Context: ctx,
		Body: &models.BootSource{
			KernelImagePath: strPtr(kernelPath),
			BootArgs:        bootArgs,
		},
	})
	if err != nil {
		return fmt.Errorf("firecracker PUT /boot-source: %w", err)
	}
	return nil
}

func (a *fcAPI) putDrive(ctx context.Context, driveID, pathOnHost string) error {
	_, err := a.c.Operations.PutGuestDriveByID(&operations.PutGuestDriveByIDParams{
		Context: ctx,
		DriveID: driveID,
		Body: &models.Drive{
			DriveID:      strPtr(driveID),
			PathOnHost:   strPtr(pathOnHost),
			IsRootDevice: boolPtr(true),
			IsReadOnly:   boolPtr(false),
			CacheType:    strPtr("Unsafe"),
			IoEngine:     strPtr("Sync"),
		},
	})
	if err != nil {
		return fmt.Errorf("firecracker PUT /drives/%s: %w", driveID, err)
	}
	return nil
}

func (a *fcAPI) putMachineConfig(ctx context.Context, vcpus, memMib int) error {
	_, err := a.c.Operations.PutMachineConfiguration(&operations.PutMachineConfigurationParams{
		Context: ctx,
		Body: &models.MachineConfiguration{
			VcpuCount:  int64Ptr(int64(vcpus)),
			MemSizeMib: int64Ptr(int64(memMib)),
			Smt:        boolPtr(false),
		},
	})
	if err != nil {
		return fmt.Errorf("firecracker PUT /machine-config: %w", err)
	}
	return nil
}

func (a *fcAPI) putNetworkInterface(ctx context.Context, ifaceID, hostDev, mac string) error {
	_, err := a.c.Operations.PutGuestNetworkInterfaceByID(&operations.PutGuestNetworkInterfaceByIDParams{
		Context: ctx,
		IfaceID: ifaceID,
		Body: &models.NetworkInterface{
			IfaceID:     strPtr(ifaceID),
			HostDevName: strPtr(hostDev),
			GuestMac:    mac,
		},
	})
	if err != nil {
		return fmt.Errorf("firecracker PUT /network-interfaces/%s: %w", ifaceID, err)
	}
	return nil
}

func (a *fcAPI) startInstance(ctx context.Context) error {
	action := "InstanceStart"
	_, err := a.c.Operations.CreateSyncAction(&operations.CreateSyncActionParams{
		Context: ctx,
		Info:    &models.InstanceActionInfo{ActionType: &action},
	})
	if err != nil {
		return fmt.Errorf("firecracker PUT /actions: %w", err)
	}
	return nil
}

func strPtr(s string) *string { return &s }
func boolPtr(b bool) *bool    { return &b }
func int64Ptr(v int64) *int64 { return &v }

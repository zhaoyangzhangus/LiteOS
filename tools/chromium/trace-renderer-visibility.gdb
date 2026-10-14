set pagination off
target remote /tmp/system-render.sock
# Validated against the current Chrome build; PIE bias is 0x40000000.
hbreak *0x4fe93600
commands
silent
printf "WidgetBase::WasShown cr3=%#lx this=%#lx\n", $cr3, $rdi
continue
end
hbreak *0x4fe92a10
commands
silent
printf "WidgetBase::SetCompositorVisible cr3=%#lx this=%#lx visible=%d\n", $cr3, $rdi, $esi
continue
end
hbreak *0x4d7a8440
commands
silent
printf "AsyncLayerTreeFrameSink::SubmitCompositorFrame cr3=%#lx this=%#lx\n", $cr3, $rdi
continue
end
continue
delete breakpoints
detach
quit

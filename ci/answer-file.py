#!/usr/bin/env python3
"""Fills the answer-file template of ci/install-vm.sh.

Usage: answer-file.py <template> <x64|arm64> <mbr|gpt> <image index> <product key element or empty> [legacy]
Prints the answer file with CRLF line ends. MIT License.
"""
import sys

MBR = """     <CreatePartitions><CreatePartition wcm:action="add"><Order>1</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition></CreatePartitions>
     <ModifyPartitions><ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>NTFS</Format><Label>OS</Label><Active>true</Active></ModifyPartition></ModifyPartitions>"""
GPT = """     <CreatePartitions>
      <CreatePartition wcm:action="add"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>
      <CreatePartition wcm:action="add"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>
      <CreatePartition wcm:action="add"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>
     </CreatePartitions>
     <ModifyPartitions>
      <ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>EFI</Label></ModifyPartition>
      <ModifyPartition wcm:action="add"><Order>2</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>OS</Label></ModifyPartition>
     </ModifyPartitions>"""

template, arch, layout, index, key = sys.argv[1:6]
legacy = len(sys.argv) > 6 and sys.argv[6] != ""
text = open(template, encoding="utf-8").read()
text = text.replace("@ARCH@", "amd64" if arch == "x64" else "arm64")
text = text.replace("@PARTITIONS@", GPT if layout == "gpt" else MBR)
text = text.replace("@OSPARTITION@", "3" if layout == "gpt" else "1")
text = text.replace("@INDEX@", index).replace("@KEY@", key)
if legacy:
    text = text.replace("<HideOnlineAccountScreens>true</HideOnlineAccountScreens>", "")
sys.stdout.write(text.replace("\r\n", "\n").replace("\n", "\r\n"))

---
# SPDX-FileCopyrightText: 2024 DESY and the Constellation authors
# SPDX-License-Identifier: CC-BY-4.0 OR EUPL-1.2
title: "RandomTransmitter"
description: "A satellite that transmits random data"
category: "Developer Tools"
---

## Building

The RandomTransmitter satellite has no additional dependencies.
The satellite is not build by default, building can be enabled via:

```sh
meson configure build -Dsatellite_random_transmitter=true
```

## Parameters

| Parameter | Type | Description | Default Value |
|-----------|------|-------------|---------------|
| `pregen` | Bool | Use pre-generated data | `false` |
| `seed` | Unsigned 32-bit integer | Seed for the random engine | Random |
| `frame_size` | Unsigned integer | Size of a data frame in bytes | `1024` |
| `number_of_frames` | Unsigned integer | Number of data frames per data message | `1` |

#  Copyright (C) 2020 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""
Contains a SnapshotService that can talk to the emulator snapshot service on the given grpc port.
"""
import logging
import os
from pathlib import Path

import grpc
from aemu.discovery.emulator_discovery import EmulatorDiscovery, get_default_emulator
from aemu.proto.snapshot_service_pb2 import (
    SnapshotDetails,
    SnapshotFilter,
    SnapshotPackage,
    SnapshotUpdateDescription,
)
from aemu.proto.snapshot_service_pb2_grpc import SnapshotServiceStub
from google.protobuf import empty_pb2
from google.protobuf.wrappers_pb2 import StringValue
from tqdm import tqdm

_EMPTY_ = empty_pb2.Empty()


async def async_map(iterator, f):
    async for item in iterator:
        yield await f(item)


class SnapshotService:
    """A SnapshotService can be used to manipulate snapshots in the emulator."""

    def __init__(self, port=None, snapshot_service=None, logger=logging.getLogger()):
        """Connect to the emulator on the given port, and log on the given logger."""
        if port and not snapshot_service:
            emulator = EmulatorDiscovery().find_emulator("grpc.port", port)
            if not emulator:
                emulator = EmulatorDiscovery.connection(port)
            snapshot_service = SnapshotServiceStub(emulator.get_grpc_channel())
        if not snapshot_service:
            snapshot_service = SnapshotServiceStub(
                get_default_emulator().get_grpc_channel()
            )

        self.stub = snapshot_service
        self.logger = logger

    def _exec_unary_grpc(self, method, req):
        """Executes the given request on the service endpoint with the provided parameter,

        Returns: True on success, or False otherwise.
        """
        self.logger.debug("Executing %s(%s)", method, req)
        method_to_call = getattr(self.stub, method)
        try:
            msg = method_to_call(req)
            self.logger.debug("Response %s", msg)
            if not msg.success:
                self.logger.error("Failed to %s snapshot: %s", method, msg.err)
            return msg.success
        except grpc._channel._Rendezvous as err:
            self.logger.error("Low level grpc error: %s ", err)

        return False

    def pull(self, snap_id, dest, fmt=SnapshotPackage.Format.TAR):
        """Pulls a snapshot with the given id to the given destination dir as id.tar.gz."""
        fn = snap_id + ".tar"
        if fmt == SnapshotPackage.Format.TARGZ:
            fn += ".gz"
        fname = os.path.join(dest, fn)
        self.logger.debug("Pulling %s -> %s", snap_id, fname)
        snap = self.info(snap_id)
        total_size = sum([img.size for img in snap.details.images if img.present])

        try:
            it = self.stub.PullSnapshot(
                SnapshotPackage(snapshot_id=snap_id, format=fmt)
            )
            with open(fname, "wb") as fn:
                with tqdm(fn, total=total_size, unit="B", unit_scale=True) as t:
                    for msg in it:
                        if not msg.success:
                            self.logger.error("Failed to pull snapshot: %s", msg.err)
                            return False
                        fn.write(msg.payload)
                        t.update(len(msg.payload))
        except grpc._channel._Rendezvous as err:
            self.logger.error("Low level grpc error: %s ", err)
        self.logger.debug("Response %s", msg)
        return msg and msg.success, fname

    def push(self, src):
        """Pushes a snapshot from the given src (tar.gz) as 'snap_id'.tar.gz

        The snap_id.tar.gz should have a snaphost that has the id: snap_id.
        So foo.tar.gz should contain the snapshot named 'foo'.
        """

        def read_in_chunks(file_object, chunk_size=(256 * 1024)):
            """Ye olde python chunk reader."""
            while True:
                data = file_object.read(chunk_size)
                if not data:
                    break
                yield data

        def push_snap_iterator(fname):
            """An iterator that returns:

            1. A message containing only the id.
            2. A stream of byte objects from the tar (.gz) file.
            """
            fmt = SnapshotPackage.Format.TAR
            if fname.endswith(".tar.gz"):
                fmt = SnapshotPackage.Format.TARGZ

            total_size = os.path.getsize(fname)
            snap_id = os.path.basename(fname).replace(".gz", "").replace(".tar", "")

            with tqdm(total=total_size, unit="B", unit_scale=True) as t:
                yield SnapshotPackage(snapshot_id=snap_id, format=fmt)
                with open(fname, "rb") as snap:
                    for chunk in read_in_chunks(snap):
                        t.update(len(chunk))
                        yield SnapshotPackage(payload=chunk)

        return self._exec_unary_grpc("PushSnapshot", push_snap_iterator(src))

    def info(self, snap_id):
        """Lists all available snapshots."""
        self.logger.debug("Retrieving snapshots")
        response = self.stub.ListSnapshots(
            SnapshotFilter(statusFilter=SnapshotFilter.All)
        )
        self.logger.debug("Response %s", response)
        return next(
            (f for f in response.snapshots if f.snapshot_id == snap_id),
            None,
        )

    def lists(self):
        """Lists all available snapshots."""
        self.logger.debug("Retrieving snapshots")
        response = self.stub.ListSnapshots(
            SnapshotFilter(statusFilter=SnapshotFilter.All)
        )
        self.logger.debug("Response %s", response)
        return [f for f in response.snapshots]

    def load(self, snap_id):
        """Loads a snapshot inside the emulator."""
        return self._exec_unary_grpc(
            "LoadSnapshot", SnapshotPackage(snapshot_id=snap_id)
        )

    def save(self, snap_id):
        """Saves a snapshot inside the emulator."""
        return self._exec_unary_grpc(
            "SaveSnapshot", SnapshotPackage(snapshot_id=snap_id)
        )

    def delete(self, snap_id):
        """Deletes the given snapshot from the emulator."""
        return self._exec_unary_grpc(
            "DeleteSnapshot", SnapshotPackage(snapshot_id=snap_id)
        )

    def update(self, snap_id, description, logical_name) -> SnapshotDetails:
        """Updates the given snapshot with the given description."""
        update = SnapshotUpdateDescription(
            snapshot_id=snap_id,
            description=StringValue(value=description),
            logical_name=StringValue(value=logical_name),
        )
        return self._exec_unary_grpc("UpdateSnapshot", update)


class AsyncSnapshotService:
    """A SnapshotService can be used to manipulate snapshots in the emulator."""

    def __init__(self, port=None, snapshot_service=None, logger=logging.getLogger()):
        """Connect to the emulator on the given port, and log on the given logger."""
        if port and not snapshot_service:
            emulator = EmulatorDiscovery().find_emulator("grpc.port", port)
            if not emulator:
                emulator = EmulatorDiscovery.connection(port)
            snapshot_service = SnapshotServiceStub(emulator.get_async_grpc_channel())
        if not snapshot_service:
            snapshot_service = SnapshotServiceStub(
                get_default_emulator().get_async_grpc_channel()
            )

        self.stub = snapshot_service
        self.logger = logger

    async def pull(self, snap_id, dest, fmt=SnapshotPackage.Format.TAR):
        """Pulls a snapshot with the given id to the given destination dir as id.tar.gz."""
        fn = snap_id + ".tar"
        if fmt == SnapshotPackage.Format.TARGZ:
            fn += ".gz"
        fname = Path(dest) / fn
        self.logger.debug("Pulling %s -> %s", snap_id, fname)
        snap = await self.info(snap_id)
        total_size = sum([img.size for img in snap.details.images if img.present])

        try:
            it = self.stub.PullSnapshot(
                SnapshotPackage(snapshot_id=snap_id, format=fmt)
            )
            with open(fname, "wb") as fn:
                with tqdm(fn, total=total_size, unit="B", unit_scale=True) as t:
                    async for msg in it:
                        if not msg.success:
                            self.logger.error("Failed to pull snapshot: %s", msg.err)
                            return False
                        fn.write(msg.payload)
                        t.update(len(msg.payload))
        except Exception as err:
            self.logger.error("Low level grpc error: %s ", err)
        self.logger.debug("Response %s", msg)
        return msg and msg.success, fname

    async def push(self, src):
        """Pushes a snapshot from the given src (tar.gz) as 'snap_id'.tar.gz

        The snap_id.tar.gz should have a snaphost that has the id: snap_id.
        So foo.tar.gz should contain the snapshot named 'foo'.
        """

        def read_in_chunks(file_object, chunk_size=(256 * 1024)):
            """Ye olde python chunk reader."""
            while True:
                data = file_object.read(chunk_size)
                if not data:
                    break
                yield data

        def push_snap_iterator(fname):
            """An iterator that returns:

            1. A message containing only the id.
            2. A stream of byte objects from the tar (.gz) file.
            """
            fmt = SnapshotPackage.Format.TAR
            if fname.endswith(".tar.gz"):
                fmt = SnapshotPackage.Format.TARGZ

            total_size = os.path.getsize(fname)
            snap_id = os.path.basename(fname).replace(".gz", "").replace(".tar", "")

            with tqdm(total=total_size, unit="B", unit_scale=True) as t:
                yield SnapshotPackage(snapshot_id=snap_id, format=fmt)
                with open(fname, "rb") as snap:
                    for chunk in read_in_chunks(snap):
                        t.update(len(chunk))
                        yield SnapshotPackage(payload=chunk)

        try:
            msg = await self.stub.PushSnapshot(push_snap_iterator(src))
            if not msg.success:
                self.logger.error("Failed to push snapshot: %s", snap_id)
            return msg.success
        except Exception as e:
            self.logger.error("Failed to push snapshot %s due to %s", snap_id, e)
            return False

    async def info(self, snap_id):
        """Lists all available snapshots."""
        self.logger.debug("Retrieving snapshots")
        response = await self.stub.ListSnapshots(
            SnapshotFilter(statusFilter=SnapshotFilter.All)
        )
        self.logger.debug("Response %s", response)
        return next(
            (f for f in response.snapshots if f.snapshot_id == snap_id),
            None,
        )

    async def lists(self):
        """Lists all available snapshots."""
        self.logger.debug("Retrieving snapshots")
        response = await self.stub.ListSnapshots(
            SnapshotFilter(statusFilter=SnapshotFilter.All)
        )
        self.logger.debug("Response %s", response)
        return [f for f in response.snapshots]

    async def load(self, snap_id):
        """Loads a snapshot inside the emulator."""
        try:
            msg = await self.stub.LoadSnapshot(SnapshotPackage(snapshot_id=snap_id))
            if not msg.success:
                self.logger.error("Failed to load snapshot: %s", snap_id)
            return msg.success
        except Exception as e:
            self.logger.error("Failed to load snapshot %s due to %s", snap_id, e)
            return False

    async def save(self, snap_id):
        """Saves a snapshot inside the emulator."""
        try:
            msg = await self.stub.SaveSnapshot(SnapshotPackage(snapshot_id=snap_id))
            if not msg.success:
                self.logger.error("Failed to save snapshot: %s", snap_id)
            return msg.success
        except Exception as e:
            self.logger.error("Failed to save snapshot %s due to %s", snap_id, e)
            return False

    async def delete(self, snap_id):
        """Deletes the given snapshot from the emulator."""
        try:
            msg = await self.stub.DeleteSnapshot(SnapshotPackage(snapshot_id=snap_id))
            if not msg.success:
                self.logger.error("Failed to delete snapshot: %s", snap_id)
            return msg.success
        except Exception as e:
            self.logger.error("Failed to delete snapshot %s due to %s", snap_id, e)
            return False

    async def update(self, snap_id, description, logical_name) -> SnapshotDetails:
        """Updates the given snapshot with the given description."""
        update = SnapshotUpdateDescription(
            snapshot_id=snap_id,
            description=StringValue(value=description),
            logical_name=StringValue(value=logical_name),
        )
        return await self.stub.UpdateSnapshot(update)

# The MIT License (MIT)
#
# Copyright (c) 2017 Philippe Proulx <pproulx@efficios.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

from bt2 import native_bt, object, utils
import bt2.clock_snapshot
import collections
import bt2.packet
import bt2.stream
import bt2.event
import copy
import bt2


def _create_from_ptr(ptr):
    msg_type = native_bt.message_get_type(ptr)
    cls = None

    if msg_type not in _MESSAGE_TYPE_TO_CLS:
        raise bt2.Error('unknown message type: {}'.format(msg_type))

    return _MESSAGE_TYPE_TO_CLS[msg_type]._create_from_ptr(ptr)


def _msg_types_from_msg_classes(message_types):
    if message_types is None:
        msg_types = None
    else:
        for msg_cls in message_types:
            if msg_cls not in _MESSAGE_TYPE_TO_CLS.values():
                raise ValueError("'{}' is not a message class".format(msg_cls))

        msg_types = [msg_cls._TYPE for msg_cls in message_types]

    return msg_types


class _Message(object._SharedObject):
    _get_ref = staticmethod(native_bt.message_get_ref)
    _put_ref = staticmethod(native_bt.message_put_ref)


class _CopyableMessage(_Message):
    def __copy__(self):
        return self._copy(lambda obj: obj)

    def __deepcopy__(self, memo):
        cpy = self._copy(copy.deepcopy)
        memo[id(self)] = cpy
        return cpy


class _EventMessage(_CopyableMessage):
    _TYPE = native_bt.MESSAGE_TYPE_EVENT

    @property
    def event(self):
        event_ptr = native_bt.message_event_borrow_event(self._ptr)
        assert event_ptr is not None
        return bt2.event._Event._create_from_ptr_and_get_ref(
            event_ptr, self._ptr, self._get_ref, self._put_ref)

    @property
    def default_clock_snapshot(self):
        if self.event.event_class.stream_class.default_clock_class is None:
            return None

        snapshot_ptr = native_bt.message_event_borrow_default_clock_snapshot_const(self._ptr)

        return bt2.clock_snapshot._ClockSnapshot._create_from_ptr_and_get_ref(
            snapshot_ptr, self._ptr, self._get_ref, self._put_ref)

    @property
    def clock_class_priority_map(self):
        cc_prio_map_ptr = native_bt.message_event_get_clock_class_priority_map(self._ptr)
        assert(cc_prio_map_ptr)
        return bt2.clock_class_priority_map.ClockClassPriorityMap._create_from_ptr(cc_prio_map_ptr)

    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        self_props = (
            self.event,
            self.clock_class_priority_map,
        )
        other_props = (
            other.event,
            other.clock_class_priority_map,
        )
        return self_props == other_props

    def _copy(self, copy_func):
        # We can always use references here because those properties are
        # frozen anyway if they are part of a message. Since the
        # user cannot modify them after copying the message, it's
        # useless to copy/deep-copy them.
        return EventMessage(self.event, self.clock_class_priority_map)


class _PacketBeginningMessage(_CopyableMessage):
    _TYPE = native_bt.MESSAGE_TYPE_PACKET_BEGINNING

    @property
    def packet(self):
        packet_ptr = native_bt.message_packet_begin_get_packet(self._ptr)
        assert(packet_ptr)
        return bt2.packet._Packet._create_from_ptr(packet_ptr)

    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        return self.packet == other.packet

    def _copy(self, copy_func):
        # We can always use references here because those properties are
        # frozen anyway if they are part of a message. Since the
        # user cannot modify them after copying the message, it's
        # useless to copy/deep-copy them.
        return PacketBeginningMessage(self.packet)


class _PacketEndMessage(_CopyableMessage):
    _TYPE = native_bt.MESSAGE_TYPE_PACKET_END

    @property
    def packet(self):
        packet_ptr = native_bt.message_packet_end_get_packet(self._ptr)
        assert(packet_ptr)
        return bt2.packet._Packet._create_from_ptr(packet_ptr)

    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        return self.packet == other.packet

    def _copy(self, copy_func):
        # We can always use references here because those properties are
        # frozen anyway if they are part of a message. Since the
        # user cannot modify them after copying the message, it's
        # useless to copy/deep-copy them.
        return PacketEndMessage(self.packet)


class _StreamBeginningMessage(_CopyableMessage):
    _TYPE = native_bt.MESSAGE_TYPE_STREAM_BEGINNING

    @property
    def stream(self):
        stream_ptr = native_bt.message_stream_begin_get_stream(self._ptr)
        assert(stream_ptr)
        return bt2.stream._create_from_ptr(stream_ptr)

    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        return self.stream == other.stream

    def _copy(self, copy_func):
        # We can always use references here because those properties are
        # frozen anyway if they are part of a message. Since the
        # user cannot modify them after copying the message, it's
        # useless to copy/deep-copy them.
        return StreamBeginningMessage(self.stream)


class _StreamEndMessage(_CopyableMessage):
    _TYPE = native_bt.MESSAGE_TYPE_STREAM_END

    @property
    def stream(self):
        stream_ptr = native_bt.message_stream_end_get_stream(self._ptr)
        assert(stream_ptr)
        return bt2.stream._create_from_ptr(stream_ptr)

    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        return self.stream == other.stream

    def _copy(self, copy_func):
        # We can always use references here because those properties are
        # frozen anyway if they are part of a message. Since the
        # user cannot modify them after copying the message, it's
        # useless to copy/deep-copy them.
        return StreamEndMessage(self.stream)


class _InactivityMessageClockSnapshotsIterator(collections.abc.Iterator):
    def __init__(self, msg_clock_snapshots):
        self._msg_clock_snapshots = msg_clock_snapshots
        self._clock_classes = list(msg_clock_snapshots._msg.clock_class_priority_map)
        self._at = 0

    def __next__(self):
        if self._at == len(self._clock_classes):
            raise StopIteration

        self._at += 1
        return self._clock_classes[at]


class _InactivityMessageClockSnapshots(collections.abc.Mapping):
    def __init__(self, msg):
        self._msg = msg

    def __getitem__(self, clock_class):
        utils._check_type(clock_class, bt2.ClockClass)
        clock_snapshot_ptr = native_bt.message_inactivity_get_clock_snapshot(self._msg._ptr,
                                                                            clock_class._ptr)

        if clock_snapshot_ptr is None:
            return

        clock_snapshot = bt2.clock_snapshot._create_clock_snapshot_from_ptr(clock_snapshot_ptr)
        return clock_snapshot

    def add(self, clock_snapshot):
        utils._check_type(clock_snapshot, bt2.clock_snapshot._ClockSnapshot)
        ret = native_bt.message_inactivity_set_clock_snapshot(self._msg._ptr,
                                                                clock_snapshot._ptr)
        utils._handle_ret(ret, "cannot set inactivity message object's clock value")

    def __len__(self):
        return len(self._msg.clock_class_priority_map)

    def __iter__(self):
        return _InactivityMessageClockSnapshotsIterator(self)


class InactivityMessage(_CopyableMessage):
    _TYPE = native_bt.MESSAGE_TYPE_MESSAGE_ITERATOR_INACTIVITY

    def __init__(self, cc_prio_map=None):
        if cc_prio_map is not None:
            utils._check_type(cc_prio_map, bt2.clock_class_priority_map.ClockClassPriorityMap)
            cc_prio_map_ptr = cc_prio_map._ptr
        else:
            cc_prio_map_ptr = None

        ptr = native_bt.message_inactivity_create(cc_prio_map_ptr)

        if ptr is None:
            raise bt2.CreationError('cannot create inactivity message object')

        super().__init__(ptr)

    @property
    def clock_class_priority_map(self):
        cc_prio_map_ptr = native_bt.message_inactivity_get_clock_class_priority_map(self._ptr)
        assert(cc_prio_map_ptr)
        return bt2.clock_class_priority_map.ClockClassPriorityMap._create_from_ptr(cc_prio_map_ptr)

    @property
    def clock_snapshots(self):
        return _InactivityMessageClockSnapshots(self)

    def _get_clock_snapshots(self):
        clock_snapshots = {}

        for clock_class, clock_snapshot in self.clock_snapshots.items():
            if clock_snapshot is None:
                continue

            clock_snapshots[clock_class] = clock_snapshot

        return clock_snapshots

    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        self_props = (
            self.clock_class_priority_map,
            self._get_clock_snapshots(),
        )
        other_props = (
            other.clock_class_priority_map,
            other._get_clock_snapshots(),
        )
        return self_props == other_props

    def __copy__(self):
        cpy = InactivityMessage(self.clock_class_priority_map)

        for clock_class, clock_snapshot in self.clock_snapshots.items():
            if clock_snapshot is None:
                continue

            cpy.clock_snapshots.add(clock_snapshot)

        return cpy

    def __deepcopy__(self, memo):
        cc_prio_map_cpy = copy.deepcopy(self.clock_class_priority_map)
        cpy = InactivityMessage(cc_prio_map_cpy)

        # copy clock values
        for orig_clock_class in self.clock_class_priority_map:
            orig_clock_snapshot = self.clock_snapshot(orig_clock_class)

            if orig_clock_snapshot is None:
                continue

            # find equivalent, copied clock class in CC priority map copy
            for cpy_clock_class in cc_prio_map_cpy:
                if cpy_clock_class == orig_clock_class:
                    break

            # create copy of clock value from copied clock class
            clock_snapshot_cpy = cpy_clock_class(orig_clock_snapshot.cycles)

            # set copied clock value in message copy
            cpy.clock_snapshots.add(clock_snapshot_cpy)

        memo[id(self)] = cpy
        return cpy


class _DiscardedElementsMessage(_Message):
    def __eq__(self, other):
        if type(other) is not type(self):
            return False

        if self.addr == other.addr:
            return True

        self_props = (
            self.count,
            self.stream,
            self.beginning_clock_snapshot,
            self.end_clock_snapshot,
        )
        other_props = (
            other.count,
            other.stream,
            other.beginning_clock_snapshot,
            other.end_clock_snapshot,
        )
        return self_props == other_props


class _DiscardedPacketsMessage(_DiscardedElementsMessage):
    _TYPE = native_bt.MESSAGE_TYPE_DISCARDED_PACKETS

    @property
    def count(self):
        count = native_bt.message_discarded_packets_get_count(self._ptr)
        assert(count >= 0)
        return count

    @property
    def stream(self):
        stream_ptr = native_bt.message_discarded_packets_get_stream(self._ptr)
        assert(stream_ptr)
        return bt2.stream._create_from_ptr(stream_ptr)

    @property
    def beginning_clock_snapshot(self):
        clock_snapshot_ptr = native_bt.message_discarded_packets_get_begin_clock_snapshot(self._ptr)

        if clock_snapshot_ptr is None:
            return

        clock_snapshot = bt2.clock_snapshot._create_clock_snapshot_from_ptr(clock_snapshot_ptr)
        return clock_snapshot

    @property
    def end_clock_snapshot(self):
        clock_snapshot_ptr = native_bt.message_discarded_packets_get_end_clock_snapshot(self._ptr)

        if clock_snapshot_ptr is None:
            return

        clock_snapshot = bt2.clock_snapshot._create_clock_snapshot_from_ptr(clock_snapshot_ptr)
        return clock_snapshot


class _DiscardedEventsMessage(_DiscardedElementsMessage):
    _TYPE = native_bt.MESSAGE_TYPE_DISCARDED_EVENTS

    @property
    def count(self):
        count = native_bt.message_discarded_events_get_count(self._ptr)
        assert(count >= 0)
        return count

    @property
    def stream(self):
        stream_ptr = native_bt.message_discarded_events_get_stream(self._ptr)
        assert(stream_ptr)
        return bt2.stream._create_from_ptr(stream_ptr)

    @property
    def beginning_clock_snapshot(self):
        clock_snapshot_ptr = native_bt.message_discarded_events_get_begin_clock_snapshot(self._ptr)

        if clock_snapshot_ptr is None:
            return

        clock_snapshot = bt2.clock_snapshot._create_clock_snapshot_from_ptr(clock_snapshot_ptr)
        return clock_snapshot

    @property
    def end_clock_snapshot(self):
        clock_snapshot_ptr = native_bt.message_discarded_events_get_end_clock_snapshot(self._ptr)

        if clock_snapshot_ptr is None:
            return

        clock_snapshot = bt2.clock_snapshot._create_clock_snapshot_from_ptr(clock_snapshot_ptr)
        return clock_snapshot


_MESSAGE_TYPE_TO_CLS = {
    native_bt.MESSAGE_TYPE_EVENT: _EventMessage,
    native_bt.MESSAGE_TYPE_PACKET_BEGINNING: _PacketBeginningMessage,
    native_bt.MESSAGE_TYPE_PACKET_END: _PacketEndMessage,
    native_bt.MESSAGE_TYPE_STREAM_BEGINNING: _StreamBeginningMessage,
    native_bt.MESSAGE_TYPE_STREAM_END: _StreamEndMessage,
    native_bt.MESSAGE_TYPE_MESSAGE_ITERATOR_INACTIVITY: InactivityMessage,
    native_bt.MESSAGE_TYPE_DISCARDED_PACKETS: _DiscardedPacketsMessage,
    native_bt.MESSAGE_TYPE_DISCARDED_EVENTS: _DiscardedEventsMessage,
}

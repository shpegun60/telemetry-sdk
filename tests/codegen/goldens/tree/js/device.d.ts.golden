// Generated telemetry v3.0 contract. SPDX-License-Identifier: MIT.
export type ByteInput = ArrayBuffer | ArrayBufferView;
export interface ValueCodec<T> {
  readonly wireBytes: number;
  encode(value: T): Uint8Array;
  /** May write an aggregate prefix before a later member is rejected. */
  encodeInto(value: T, output: ByteInput): void;
  decode(input: ByteInput): T;
}
export type Type_0 = undefined;
export type Type_1 = boolean;
export type Type_2 = number;
export type Type_4 = number;
export type Type_5 = number;
export type Type_6 = number;
export type Type_10 = number;
export interface Type_12 {
  "gain": Type_10;
  "rateHz": Type_4;
  "enabled": Type_1;
}
export type Type_13 = ReadonlyArray<Type_5> & { readonly length: 2 };
export interface Type_14 {
  "flags": Type_4;
  "raw": Type_13;
}
export type Type_15 = Type_2;
export interface Type_16 {
  "cycles": Type_6;
  "window": Type_15;
}
export interface Type_17 {
  "volts": Type_10;
}
export interface Type_18 {
  "counts": Type_10;
}
export declare class TelemetryCodecError extends Error { constructor(reason: string); }
export declare const fingerprint: 16645511211320830712n;
export declare const descriptorSha256: string;
export declare const wireVersion: Readonly<{major: 3; minor: 1}>;
export declare const Operation: Readonly<{FieldRead: 'FieldRead'; FieldWrite: 'FieldWrite'; Command: 'Command'; Service: 'Service'}>;
export declare function assertCompatibleFingerprint(peer: bigint): void;
export declare function matchesHeader(input: ByteInput): boolean;
export declare const codecs: Readonly<{
  Type_0: ValueCodec<Type_0>;
  Type_1: ValueCodec<Type_1>;
  Type_2: ValueCodec<Type_2>;
  Type_4: ValueCodec<Type_4>;
  Type_5: ValueCodec<Type_5>;
  Type_6: ValueCodec<Type_6>;
  Type_10: ValueCodec<Type_10>;
  Type_12: ValueCodec<Type_12>;
  Type_13: ValueCodec<Type_13>;
  Type_14: ValueCodec<Type_14>;
  Type_15: ValueCodec<Type_15>;
  Type_16: ValueCodec<Type_16>;
  Type_17: ValueCodec<Type_17>;
  Type_18: ValueCodec<Type_18>;
}>;
export declare const enums: Readonly<{
  Type_15: Readonly<{readonly "Rectangular": 0; readonly "Hann": 1; readonly "FlatTop": 2}>;
}>;
export declare namespace types {
  namespace adc {
    type Config = Type_12;
    const Config: typeof codecs.Type_12;
    type Status = Type_14;
    const Status: typeof codecs.Type_14;
    type Reference = Type_17;
    const Reference: typeof codecs.Type_17;
    type Offset = Type_18;
    const Offset: typeof codecs.Type_18;
  }
  namespace metrology {
    type Window = Type_15;
    const Window: typeof enums.Type_15;
    type Config = Type_16;
    const Config: typeof codecs.Type_16;
  }
}
export declare namespace adc {
  namespace types {
    type Config = Type_12;
    const Config: typeof codecs.Type_12;
    type Status_value = Type_14;
    const Status_value: typeof codecs.Type_14;
    type ResetRequest = Type_0;
    const ResetRequest: typeof codecs.Type_0;
    type CalibrateRequest = Type_17;
    const CalibrateRequest: typeof codecs.Type_17;
    type CalibrateResponse = Type_18;
    const CalibrateResponse: typeof codecs.Type_18;
  }
  const fields: Readonly<{
    readonly "Config": Readonly<{readonly id: 0; readonly category: 1; readonly catalogName: string; readonly name: string; readonly path: string; readonly valueTypeId: 12; readonly capabilities: 3; readonly read: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldRead'; id: 0; typeId: 12; requestBytes: 0; responseBytes: 7; decode: ValueCodec<Type_12>['decode']}>; readonly write: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldWrite'; id: 0; typeId: 12; requestBytes: 7; responseBytes: 0; encode: ValueCodec<Type_12>['encode']; encodeInto: ValueCodec<Type_12>['encodeInto']}>}>;
    readonly "Status_value": Readonly<{readonly id: 1; readonly category: 1; readonly catalogName: string; readonly name: string; readonly path: string; readonly valueTypeId: 14; readonly capabilities: 1; readonly read: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldRead'; id: 1; typeId: 14; requestBytes: 0; responseBytes: 6; decode: ValueCodec<Type_14>['decode']}>}>;
  }>;
  const commands: Readonly<{
    readonly "Reset": Readonly<{readonly id: 0; readonly category: 2; readonly catalogName: string; readonly name: string; readonly path: string; readonly fingerprint: typeof fingerprint; readonly operation: 'Command'; readonly requestTypeId: 0; readonly requestBytes: 0; readonly responseBytes: 0; readonly encode: ValueCodec<Type_0>['encode']; readonly encodeInto: ValueCodec<Type_0>['encodeInto']}>;
  }>;
  const services: Readonly<{
    readonly "Calibrate": Readonly<{readonly id: 0; readonly category: 3; readonly catalogName: string; readonly name: string; readonly path: string; readonly fingerprint: typeof fingerprint; readonly operation: 'Service'; readonly requestTypeId: 17; readonly requestBytes: 4; readonly responseBytes: 4; readonly encode: ValueCodec<Type_17>['encode']; readonly encodeInto: ValueCodec<Type_17>['encodeInto']; readonly responseTypeId: 18; readonly decodeResponse: ValueCodec<Type_18>['decode']}>;
  }>;
  namespace channel1 {
    namespace types {
      type Gain = Type_10;
      const Gain: typeof codecs.Type_10;
    }
    const fields: Readonly<{
      readonly "Gain": Readonly<{readonly id: 65536; readonly category: 1; readonly catalogName: string; readonly name: string; readonly path: string; readonly valueTypeId: 10; readonly capabilities: 3; readonly read: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldRead'; id: 65536; typeId: 10; requestBytes: 0; responseBytes: 4; readonly readManyScalar: 10; decode: ValueCodec<Type_10>['decode']}>; readonly write: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldWrite'; id: 65536; typeId: 10; requestBytes: 4; responseBytes: 0; encode: ValueCodec<Type_10>['encode']; encodeInto: ValueCodec<Type_10>['encodeInto']}>}>;
    }>;
    const commands: Readonly<{
    }>;
    const services: Readonly<{
    }>;
    const Gain: (typeof fields)["Gain"];
  }
  namespace channel2 {
    namespace types {
      type Gain = Type_10;
      const Gain: typeof codecs.Type_10;
    }
    const fields: Readonly<{
      readonly "Gain": Readonly<{readonly id: 131072; readonly category: 1; readonly catalogName: string; readonly name: string; readonly path: string; readonly valueTypeId: 10; readonly capabilities: 3; readonly read: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldRead'; id: 131072; typeId: 10; requestBytes: 0; responseBytes: 4; readonly readManyScalar: 10; decode: ValueCodec<Type_10>['decode']}>; readonly write: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldWrite'; id: 131072; typeId: 10; requestBytes: 4; responseBytes: 0; encode: ValueCodec<Type_10>['encode']; encodeInto: ValueCodec<Type_10>['encodeInto']}>}>;
    }>;
    const commands: Readonly<{
    }>;
    const services: Readonly<{
    }>;
    const Gain: (typeof fields)["Gain"];
  }
  const Config: (typeof fields)["Config"];
  const Status_value: (typeof fields)["Status_value"];
  const Reset: (typeof commands)["Reset"];
  const Calibrate: (typeof services)["Calibrate"];
}
export declare namespace metrology {
  namespace types {
    type Config = Type_16;
    const Config: typeof codecs.Type_16;
  }
  const fields: Readonly<{
    readonly "Config": Readonly<{readonly id: 196608; readonly category: 1; readonly catalogName: string; readonly name: string; readonly path: string; readonly valueTypeId: 16; readonly capabilities: 3; readonly read: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldRead'; id: 196608; typeId: 16; requestBytes: 0; responseBytes: 5; decode: ValueCodec<Type_16>['decode']}>; readonly write: Readonly<{readonly fingerprint: typeof fingerprint; operation: 'FieldWrite'; id: 196608; typeId: 16; requestBytes: 5; responseBytes: 0; encode: ValueCodec<Type_16>['encode']; encodeInto: ValueCodec<Type_16>['encodeInto']}>}>;
  }>;
  const commands: Readonly<{
  }>;
  const services: Readonly<{
  }>;
  const Config: (typeof fields)["Config"];
}

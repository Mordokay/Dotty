import type * as React from 'react';

/** A light colour: one of the light names, or any CSS colour (the bracelet's own RGB). */
export type Light = 'firefly' | 'leaf' | 'lagoon' | 'dusk' | 'bloom' | 'ember' | 'amber' | (string & {});
export type IconName = 'play' | 'stop' | 'plus' | 'close' | 'chevron' | 'bluetooth' | 'sliders' | 'ripple' | 'wave';

export interface LightFieldProps { lights?: Light[]; motes?: number; seed?: number; parallax?: boolean; className?: string; style?: React.CSSProperties; children?: React.ReactNode }
export declare function LightField(props: LightFieldProps): React.ReactElement;

export interface LightOrbProps { light?: Light; size?: number; pulse?: boolean; label?: string; className?: string }
export declare function LightOrb(props: LightOrbProps): React.ReactElement;

export interface GlassProps { title?: string; light?: Light; label?: string; className?: string; children?: React.ReactNode }
export declare function Glass(props: GlassProps): React.ReactElement;

export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> { variant?: 'light' | 'glass' | 'quiet'; light?: Light; size?: 'm' | 's'; icon?: IconName; block?: boolean }
export declare function Button(props: ButtonProps): React.ReactElement;

export interface IconButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> { icon: IconName; label: string; variant?: 'light' | 'glass' | 'quiet'; light?: Light }
export declare function IconButton(props: IconButtonProps): React.ReactElement;

export interface ListRowProps { title: React.ReactNode; subtitle?: React.ReactNode; light?: Light; leading?: React.ReactNode; trailing?: React.ReactNode; onClick?: () => void }
export declare function ListRow(props: ListRowProps): React.ReactElement;

export interface ToggleProps { checked: boolean; onChange?: (checked: boolean) => void; label: string; showLabel?: boolean; light?: Light; disabled?: boolean }
export declare function Toggle(props: ToggleProps): React.ReactElement;

export interface SliderProps { value: number; onChange?: (value: number) => void; min?: number; max?: number; step?: number; label?: string; light?: Light; emptyLight?: Light; format?: (value: number) => string; id?: string }
export declare function Slider(props: SliderProps): React.ReactElement;

export interface ColorPickerProps { colors?: { name: string; value: Light }[]; value?: Light; onChange?: (value: Light) => void; label?: string }
export declare function ColorPicker(props: ColorPickerProps): React.ReactElement;

export interface StatusPillProps { state?: 'connected' | 'searching' | 'connecting' | 'off' | 'error'; light?: Light; children?: React.ReactNode }
export declare function StatusPill(props: StatusPillProps): React.ReactElement;

export interface FireflyLoaderProps { size?: number; light?: Light; label?: string; className?: string }
export declare function FireflyLoader(props: FireflyLoaderProps): React.ReactElement;

export interface LogoProps { size?: number; layout?: 'stack' | 'row'; wordmark?: boolean; className?: string }
export declare function Logo(props: LogoProps): React.ReactElement;

export interface IconProps { name: IconName; size?: number; label?: string; className?: string }
export declare function Icon(props: IconProps): React.ReactElement;

declare global {
  interface Window {
    Dotty: {
      LightField: typeof LightField; LightOrb: typeof LightOrb; Glass: typeof Glass;
      Button: typeof Button; IconButton: typeof IconButton; ListRow: typeof ListRow;
      Toggle: typeof Toggle; Slider: typeof Slider; ColorPicker: typeof ColorPicker; StatusPill: typeof StatusPill;
      FireflyLoader: typeof FireflyLoader; Logo: typeof Logo; Icon: typeof Icon;
      markSrc: string; lights: string[];
    };
  }
}

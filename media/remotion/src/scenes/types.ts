import type React from 'react';
import type {InteractivitySchema} from 'remotion';
export type SceneProps={title:string;subtitle:string;style?:React.CSSProperties};
export const sceneSchema={title:{type:'text-content',default:'Burnos',description:'Headline'},subtitle:{type:'text-content',default:'',description:'Supporting line'}} as const satisfies InteractivitySchema;

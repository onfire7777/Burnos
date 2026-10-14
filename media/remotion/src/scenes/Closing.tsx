import {Interactive,useCurrentFrame} from 'remotion';
import {C,Orb,Stage,Tag,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const ClosingInner=({title,subtitle,style}:SceneProps)=>{const f=useCurrentFrame();return <Stage duration={9} chapter="BURNOS / IN CONTEXT. IN MOTION." label="ALEXA+ TRACK · PRODUCT DEMO" style={style}>
 <div style={{position:'absolute',left:740,top:95,transform:`scale(${.85+enter(f)*.15})`}}><Orb size={440}/></div>
 <div style={{position:'absolute',left:0,right:0,top:505,textAlign:'center'}}><div style={{fontSize:124,fontWeight:600,letterSpacing:-8,opacity:enter(f,15)}}>{title}</div><div style={{fontSize:39,color:C.muted,marginTop:16,opacity:enter(f,45)}}>{subtitle}</div><div style={{display:'flex',gap:22,justifyContent:'center',marginTop:40,opacity:enter(f,70)}}><Tag>Remember</Tag><Tag color={C.violet}>Converse</Tag><Tag color={C.green}>Create</Tag></div><div style={{fontSize:32,color:C.cyan,marginTop:36,letterSpacing:3,opacity:enter(f,100)}}>burnos.app</div></div>
 </Stage>};
export const Closing=Interactive.withSchema({Component:ClosingInner,componentName:'<Closing>',schema:sceneSchema,wrapInSequence:true});

import {Interactive,useCurrentFrame} from 'remotion';
import {C,Orb,Stage,Tag,Heading,Card,Lines,Wave,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const ConversationInner=({title,subtitle,style}:SceneProps)=>{const f=useCurrentFrame();return <Stage duration={13} chapter="02 / TALK NATURALLY" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <Card style={{position:'absolute',left:100,top:370,width:1060,height:430,padding:0,overflow:'hidden'}}>
  <div style={{height:55,borderBottom:`1px solid ${C.line}`,padding:'16px 28px',fontSize:19,color:C.muted}}>AURORA / LAUNCH NOTES <span style={{float:'right'}}>Your workspace</span></div>
  <div style={{padding:40,width:760}}><div style={{fontSize:36,fontWeight:600,marginBottom:22}}>Aurora / A beginner design workshop</div><div style={{fontSize:25,color:C.muted,marginBottom:27}}>One idea. Twelve people. A creative afternoon.</div><Lines widths={[100,84,60]}/><div style={{display:'flex',gap:15,marginTop:36}}><Tag>Plan</Tag><Tag color={C.violet}>Create</Tag><Tag color={C.green}>Invite</Tag></div></div>
 </Card>
 <div style={{position:'absolute',left:1190,top:350}}><Orb size={430}/><Wave/></div>
 <Card at={60} accent={C.cyan} style={{position:'absolute',left:350,top:680,width:870,padding:'24px 32px',transform:`translateY(${(1-enter(f,60))*30}px)`}}><div style={{fontSize:18,color:C.cyan,letterSpacing:2,marginBottom:9}}>YOU</div><div style={{fontSize:31}}>“Burnos, let’s launch a design workshop.”</div></Card>
 <div style={{position:'absolute',left:1030,top:840,fontSize:25,color:C.violet,opacity:enter(f,190)}}>Talk • interrupt • keep going</div>
 </Stage>};
export const Conversation=Interactive.withSchema({Component:ConversationInner,componentName:'<Conversation>',schema:sceneSchema,wrapInSequence:true});

import {Interactive,useCurrentFrame} from 'remotion';
import {C,Orb,Stage,Tag,Heading,Card,Icon,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const ConnectionsInner=({title,subtitle,style}:SceneProps)=>{const f=useCurrentFrame();const nodes=[{x:130,y:370,name:'Calendar',text:'90-minute workshop draft',icon:'app'},{x:130,y:650,name:'Launch brief',text:'Agenda + $600 budget',icon:'file'},{x:1280,y:370,name:'Invitation draft',text:'Ready for your review',icon:'spark'},{x:1280,y:650,name:'Follow-up tasks',text:'Room · materials · invitations',icon:'code'}];return <Stage duration={16} chapter="08 / CONNECT YOUR TOOLS" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <svg style={{position:'absolute',left:0,top:0}} width="1920" height="1080">{nodes.map((n,i)=><g key={n.name} opacity={enter(f,30+i*28)}><path d={`M960 594 C ${n.x<900?700:1220} 594 ${n.x<900?720:1210} ${n.y+90} ${n.x<900?n.x+500:n.x} ${n.y+90}`} fill="none" stroke={i%2?C.violet:C.cyan} strokeWidth="2" strokeDasharray="7 10" strokeDashoffset={-f*.4}/></g>)}</svg>
 <div style={{position:'absolute',left:730,top:355}}><Orb size={460}/></div>
 {nodes.map((n,i)=><Card key={n.name} at={30+i*28} style={{position:'absolute',left:n.x,top:n.y,width:500,height:168,display:'flex',alignItems:'center',gap:25}} accent={i%2?C.violet:C.cyan}><Icon name={n.icon} size={48} color={i%2?C.violet:C.cyan}/><div><div style={{fontSize:31,fontWeight:600}}>{n.name}</div><div style={{fontSize:23,color:C.muted,marginTop:8}}>{n.text}</div></div></Card>)}
 <div style={{position:'absolute',left:784,top:807,display:'flex',gap:14}}><Tag>MCP</Tag><Tag color={C.violet}>Composio</Tag></div>
 </Stage>};
export const Connections=Interactive.withSchema({Component:ConnectionsInner,componentName:'<Connections>',schema:sceneSchema,wrapInSequence:true});

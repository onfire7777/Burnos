import {Interactive,useCurrentFrame} from 'remotion';
import {C,Stage,Tag,Heading,Card,Icon,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const ArchitectureInner=({title,subtitle,style}:SceneProps)=>{const f=useCurrentFrame();return <Stage duration={16} chapter="10 / BUILT FOR THE NEXT CONVERSATION" label="TECHNICAL OVERVIEW · ALEXA+ TRACK" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <div style={{position:'absolute',left:100,top:363,display:'flex',gap:28}}>
  <Card style={{width:825,height:305}} accent={C.cyan}><Tag>ON YOUR WINDOWS PC</Tag><div style={{display:'flex',gap:24,marginTop:33}}>{[['wave','Speech in / out'],['memory','Local memory'],['app','Native desktop']].map(([icon,t])=><div key={t} style={{width:240,textAlign:'center'}}><div style={{height:65,display:'grid',placeItems:'center'}}><Icon name={icon} size={42}/></div><div style={{fontSize:26,marginTop:19}}>{t}</div></div>)}</div><div style={{fontSize:21,color:C.muted,marginTop:32,textAlign:'center'}}>Parakeet · Kokoro · Mnemosyne · C++ / Qt</div></Card>
  <Card style={{width:825,height:305}} accent={C.violet} at={40}><Tag color={C.violet}>CONNECTED WHEN NEEDED</Tag><div style={{fontSize:34,fontWeight:600,marginTop:40}}>Your choice of cloud models.</div><div style={{fontSize:26,color:C.muted,lineHeight:1.6,marginTop:17}}>Reasoning · optional semantic embeddings<br/>Connected services and research</div></Card>
 </div>
 <Card at={150} accent={C.green} style={{position:'absolute',left:100,top:712,width:1680,padding:'30px 35px',display:'flex',gap:26,alignItems:'center'}}><Icon name="code" size={40} color={C.green}/><div><div style={{fontSize:30,fontWeight:600}}>Simulated assistant experience + working self-hosted MCP server</div><div style={{fontSize:23,color:C.muted,marginTop:10}}>Mnemosyne memory backend · MCP 2025-11-25 · verified capture and recall</div></div><div style={{marginLeft:'auto',opacity:enter(f,190)}}><Icon name="check" color={C.green} size={43}/></div></Card>
 </Stage>};
export const Architecture=Interactive.withSchema({Component:ArchitectureInner,componentName:'<Architecture>',schema:sceneSchema,wrapInSequence:true});
